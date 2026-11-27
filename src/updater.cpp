#include "./updater.h"

#include <shellapi.h>
#include <wininet.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <string>
#include <string_view>

#include "./config_file_util.h"
#include "./tray.h"

namespace updater {
namespace {

struct InternetHandle {
  HINTERNET handle = nullptr;
  explicit InternetHandle(HINTERNET h = nullptr) noexcept : handle(h) {}
  ~InternetHandle() noexcept {
    if (handle != nullptr) InternetCloseHandle(handle);
  }
  InternetHandle(const InternetHandle&) = delete;
  InternetHandle& operator=(const InternetHandle&) = delete;
};

std::wstring Utf8ToWide(const std::string& str) {
  if (str.empty()) return {};
  const int len = MultiByteToWideChar(CP_UTF8, 0, str.data(),
                                      static_cast<int>(str.size()), nullptr, 0);
  if (len <= 0) return {};
  std::wstring res(static_cast<size_t>(len), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()),
                      res.data(), len);
  return res;
}

std::array<int, 3> ParseSemVer(std::wstring_view tag) noexcept {
  while (!tag.empty() && (tag.front() == L'v' || tag.front() == L'V' ||
                          tag.front() == L' ')) {
    tag.remove_prefix(1);
  }
  std::array<int, 3> parts = {0, 0, 0};
  size_t idx = 0;
  int current = 0;
  bool has_digit = false;
  for (wchar_t ch : tag) {
    if (ch >= L'0' && ch <= L'9') {
      current = current * 10 + (ch - L'0');
      has_digit = true;
    } else if (ch == L'.' && idx < 2) {
      parts[idx++] = current;
      current = 0;
      has_digit = false;
    } else {
      break;
    }
  }
  if (has_digit && idx < 3) {
    parts[idx] = current;
  }
  return parts;
}

bool IsNewerVersion(std::wstring_view latest,
                    std::wstring_view current) noexcept {
  return ParseSemVer(latest) > ParseSemVer(current);
}

DWORD QueryHttpStatusCode(HINTERNET request) noexcept {
  DWORD status_code = 0;
  DWORD size = sizeof(status_code);
  if (HttpQueryInfoW(request, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                     &status_code, &size, nullptr) == FALSE) {
    return 0;
  }
  return status_code;
}

std::uint64_t QueryHttpContentLength(HINTERNET request) noexcept {
  wchar_t buf[64] = {};
  DWORD size = sizeof(buf);
  if (HttpQueryInfoW(request, HTTP_QUERY_CONTENT_LENGTH, buf, &size, nullptr) ==
      FALSE) {
    return 0;
  }
  std::uint64_t val = 0;
  for (const wchar_t* p = buf; *p != L'\0'; ++p) {
    if (*p < L'0' || *p > L'9') return 0;
    val = val * 10ULL + static_cast<std::uint64_t>(*p - L'0');
  }
  return val;
}

std::wstring ReadJsonStringAt(const std::wstring& json, size_t start_quote) {
  std::wstring result;
  bool escape = false;
  for (size_t i = start_quote + 1; i < json.size(); ++i) {
    const wchar_t c = json[i];
    if (escape) {
      switch (c) {
        case L'n': result += L'\n'; break;
        case L'r': result += L'\r'; break;
        case L't': result += L'\t'; break;
        case L'\"': result += L'\"'; break;
        case L'\\': result += L'\\'; break;
        case L'/': result += L'/'; break;
        default: result += c; break;
      }
      escape = false;
    } else if (c == L'\\') {
      escape = true;
    } else if (c == L'\"') {
      break;
    } else {
      result += c;
    }
  }
  return result;
}

std::wstring ExtractJsonField(const std::wstring& json,
                              const std::wstring& key) {
  const std::wstring target = L"\"" + key + L"\"";
  const size_t pos = json.find(target);
  if (pos == std::wstring::npos) return {};

  const size_t colon = json.find(L':', pos + target.size());
  if (colon == std::wstring::npos) return {};

  const size_t start_quote = json.find(L'\"', colon);
  if (start_quote == std::wstring::npos) return {};

  return ReadJsonStringAt(json, start_quote);
}

std::wstring ExtractAssetUrl(const std::wstring& json,
                             const std::wstring& asset_suffix) {
  size_t search_pos = json.find(L"\"assets\"");
  if (search_pos == std::wstring::npos) search_pos = 0;

  std::wstring suffix_lower = asset_suffix;
  std::transform(suffix_lower.begin(), suffix_lower.end(), suffix_lower.begin(),
                 [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });

  while ((search_pos = json.find(L"\"name\"", search_pos)) !=
         std::wstring::npos) {
    const size_t colon = json.find(L':', search_pos + 6);
    if (colon == std::wstring::npos) break;
    const size_t name_start = json.find(L'\"', colon);
    if (name_start == std::wstring::npos) break;
    const size_t name_end = json.find(L'\"', name_start + 1);
    if (name_end == std::wstring::npos) break;

    std::wstring name_lower =
        json.substr(name_start + 1, name_end - name_start - 1);
    std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });

    if (name_lower.size() >= suffix_lower.size() &&
        name_lower.compare(name_lower.size() - suffix_lower.size(),
                           suffix_lower.size(), suffix_lower) == 0) {
      const size_t url_key = json.find(L"\"browser_download_url\"", name_end);
      if (url_key != std::wstring::npos) {
        const size_t url_colon = json.find(L':', url_key);
        if (url_colon != std::wstring::npos) {
          const size_t url_start = json.find(L'\"', url_colon);
          if (url_start != std::wstring::npos) {
            return ReadJsonStringAt(json, url_start);
          }
        }
      }
    }
    search_pos = name_end + 1;
  }
  return {};
}

bool DownloadFileToDisk(const std::wstring& url_str,
                        const std::wstring& dest_path,
                        std::atomic<bool>& cancel_flag,
                        const ProgressCallback& on_progress,
                        bool* cancelled_out) noexcept {
  if (cancelled_out) *cancelled_out = false;
  if (!IsTrustedGitHubUrl(url_str)) return false;

  InternetHandle net(InternetOpenW(
      L"CrossPointer-Updater", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0));
  if (net.handle == nullptr) return false;

  DWORD timeout_ms = 25000;
  InternetSetOptionW(net.handle, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout_ms,
                     sizeof(timeout_ms));
  InternetSetOptionW(net.handle, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout_ms,
                     sizeof(timeout_ms));

  constexpr wchar_t kHeaders[] = L"User-Agent: CrossPointer-App\r\n";
  InternetHandle url(InternetOpenUrlW(
      net.handle, url_str.c_str(), kHeaders, static_cast<DWORD>(-1),
      INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE,
      0));
  if (url.handle == nullptr) return false;

  if (QueryHttpStatusCode(url.handle) != 200) return false;
  const std::uint64_t total_bytes = QueryHttpContentLength(url.handle);

  HANDLE file = CreateFileW(dest_path.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;

  char buffer[16384];
  DWORD bytes_read = 0;
  DWORD bytes_written = 0;
  std::uint64_t downloaded = 0;
  bool success = true;

  const auto start_tp = std::chrono::steady_clock::now();
  auto last_emit = start_tp - std::chrono::milliseconds(100);

  auto emit_progress = [&](bool force) {
    if (!on_progress) return;
    const auto now = std::chrono::steady_clock::now();
    if (!force && (now - last_emit) < std::chrono::milliseconds(45)) return;
    last_emit = now;

    const double elapsed =
        std::chrono::duration<double>(now - start_tp).count();
    DownloadProgress p;
    p.downloaded_bytes = downloaded;
    p.total_bytes = total_bytes;
    p.bytes_per_sec = (elapsed > 0.05) ? (static_cast<double>(downloaded) / elapsed) : 0.0;
    p.indeterminate = (total_bytes == 0);
    if (total_bytes > 0) {
      p.percent = static_cast<int>(
          std::min<std::uint64_t>(100ULL, (downloaded * 100ULL) / total_bytes));
    }
    p.stage_text = L"正在下载更新包...";
    on_progress(p);
  };

  emit_progress(true);

  for (;;) {
    if (cancel_flag.load()) {
      if (cancelled_out) *cancelled_out = true;
      success = false;
      break;
    }

    const BOOL read_ok =
        InternetReadFile(url.handle, buffer, sizeof(buffer), &bytes_read);
    if (read_ok == FALSE) {
      success = false;
      break;
    }
    if (bytes_read == 0) {
      break;
    }
    if (WriteFile(file, buffer, bytes_read, &bytes_written, nullptr) == FALSE ||
        bytes_written != bytes_read) {
      success = false;
      break;
    }
    downloaded += bytes_read;
    emit_progress(false);
  }

  if (success) {
    FlushFileBuffers(file);
    emit_progress(true);
  }
  CloseHandle(file);

  if (!success) {
    DeleteFileW(dest_path.c_str());
  }
  return success;
}

std::wstring GetSystemTarPath() {
  std::wstring sys_dir(MAX_PATH, L'\0');
  const UINT len = GetSystemDirectoryW(sys_dir.data(),
                                       static_cast<UINT>(sys_dir.size()));
  if (len == 0 || len >= sys_dir.size()) {
    return L"tar.exe";
  }
  sys_dir.resize(len);
  return sys_dir + L"\\tar.exe";
}

}  // namespace

bool IsTrustedGitHubUrl(std::wstring_view url) noexcept {
  return url.starts_with(L"https://github.com/Crucialmer/CrossPointer/") ||
         url.starts_with(L"https://objects.githubusercontent.com/");
}

void CleanupOldExecutable() noexcept {
  const std::wstring module_path = GetExecutablePath();
  if (module_path.empty()) return;

  const size_t last_slash = module_path.find_last_of(L"\\/");
  if (last_slash == std::wstring::npos) return;
  const std::wstring dir = module_path.substr(0, last_slash + 1);

  DeleteFileW((dir + L"CrossPointer.old.exe").c_str());
  DeleteFileW((dir + L"CrossPointer.new.exe").c_str());
  DeleteFileW((dir + L"CrossPointer.update.zip").c_str());

  const std::wstring tmp_dir = dir + L"CrossPointer_update_tmp";
  DeleteFileW((tmp_dir + L"\\CrossPointer.exe").c_str());
  DeleteFileW((tmp_dir + L"\\README.md").c_str());
  DeleteFileW((tmp_dir + L"\\QuickStart_Guide.txt").c_str());
  DeleteFileW((tmp_dir + L"\\CrossPointer\\CrossPointer.exe").c_str());
  DeleteFileW((tmp_dir + L"\\CrossPointer\\README.md").c_str());
  DeleteFileW((tmp_dir + L"\\CrossPointer\\QuickStart_Guide.txt").c_str());
  RemoveDirectoryW((tmp_dir + L"\\CrossPointer").c_str());
  RemoveDirectoryW(tmp_dir.c_str());
}

UpdateResult CheckUpdateSync() noexcept {
  UpdateResult res;
  try {
    InternetHandle net(InternetOpenW(L"CrossPointer-Updater",
                                     INTERNET_OPEN_TYPE_PRECONFIG, nullptr,
                                     nullptr, 0));
    if (net.handle == nullptr) {
      res.error_msg = L"无法初始化网络组件 (InternetOpen 失败)";
      return res;
    }

    DWORD timeout_ms = 8000;
    InternetSetOptionW(net.handle, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout_ms,
                       sizeof(timeout_ms));
    InternetSetOptionW(net.handle, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout_ms,
                       sizeof(timeout_ms));

    constexpr wchar_t kHeaders[] =
        L"User-Agent: CrossPointer-App\r\nAccept: "
        L"application/vnd.github+json\r\n";
    constexpr wchar_t kUrl[] =
        L"https://api.github.com/repos/Crucialmer/CrossPointer/releases/latest";

    InternetHandle url(InternetOpenUrlW(
        net.handle, kUrl, kHeaders, static_cast<DWORD>(-1),
        INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD |
            INTERNET_FLAG_NO_CACHE_WRITE,
        0));
    if (url.handle == nullptr) {
      res.error_msg = L"无法连接到 GitHub 检查更新，请检查网络或代理设置。";
      return res;
    }

    const DWORD status_code = QueryHttpStatusCode(url.handle);
    if (status_code != 200) {
      res.error_msg = L"GitHub 更新接口返回异常状态码 (HTTP " +
                      std::to_wstring(status_code) + L")，请稍后重试。";
      return res;
    }

    std::string response;
    char buffer[4096];
    DWORD bytes_read = 0;
    for (;;) {
      const BOOL read_ok =
          InternetReadFile(url.handle, buffer, sizeof(buffer), &bytes_read);
      if (read_ok == FALSE) {
        res.error_msg = L"读取更新信息时网络连接中断。";
        return res;
      }
      if (bytes_read == 0) break;
      response.append(buffer, bytes_read);
      if (response.size() > 512 * 1024) break;
    }

    if (response.empty()) {
      res.error_msg = L"GitHub 接口返回空数据。";
      return res;
    }

    const std::wstring json = Utf8ToWide(response);
    res.latest_version = ExtractJsonField(json, L"tag_name");
    res.release_url = ExtractJsonField(json, L"html_url");
    res.changelog = ExtractJsonField(json, L"body");
    res.exe_download_url = ExtractAssetUrl(json, L".exe");
    res.zip_download_url = ExtractAssetUrl(json, L".zip");

    if (res.latest_version.empty()) {
      res.error_msg = L"解析 GitHub 版本响应失败。";
      return res;
    }

    res.success = true;
    res.has_update = IsNewerVersion(res.latest_version, kCurrentVersion);
  } catch (...) {
    res.error_msg = L"检查更新时发生未知异常。";
  }
  return res;
}

bool PerformDirectUpdate(const UpdateResult& update_info, HWND controller_hwnd,
                         std::atomic<bool>& cancel_flag,
                         ProgressCallback on_progress,
                         std::wstring* error_out) noexcept {
  try {
    const std::wstring module_path = GetExecutablePath();
    if (module_path.empty()) {
      if (error_out) *error_out = L"无法获取当前程序路径。";
      return false;
    }

    const size_t last_slash = module_path.find_last_of(L"\\/");
    if (last_slash == std::wstring::npos) {
      if (error_out) *error_out = L"解析程序目录失败。";
      return false;
    }
    const std::wstring dir = module_path.substr(0, last_slash + 1);
    const std::wstring current_exe = module_path;
    const std::wstring old_exe = dir + L"CrossPointer.old.exe";
    const std::wstring new_exe = dir + L"CrossPointer.new.exe";
    const std::wstring zip_file = dir + L"CrossPointer.update.zip";

    bool download_ok = false;
    bool cancelled = false;
    if (!update_info.exe_download_url.empty()) {
      download_ok = DownloadFileToDisk(update_info.exe_download_url, new_exe,
                                       cancel_flag, on_progress, &cancelled);
    }

    if (!download_ok && !cancelled && !update_info.zip_download_url.empty()) {
      if (DownloadFileToDisk(update_info.zip_download_url, zip_file,
                             cancel_flag, on_progress, &cancelled)) {
        if (on_progress) {
          DownloadProgress p;
          p.percent = 100;
          p.indeterminate = false;
          p.installing = true;
          p.stage_text = L"正在解压更新压缩包...";
          on_progress(p);
        }

        const std::wstring tmp_dir = dir + L"CrossPointer_update_tmp";
        CreateDirectoryW(tmp_dir.c_str(), nullptr);

        const std::wstring tar_path = GetSystemTarPath();
        std::wstring extract_cmd = L"\"" + tar_path + L"\" -xf \"" + zip_file +
                                   L"\" -C \"" + tmp_dir + L"\"";
        STARTUPINFOW si = {sizeof(si)};
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi = {};
        bool tar_ok = false;
        if (CreateProcessW(nullptr, extract_cmd.data(), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi)) {
          if (WaitForSingleObject(pi.hProcess, 20000) == WAIT_OBJECT_0) {
            DWORD exit_code = 1;
            if (GetExitCodeProcess(pi.hProcess, &exit_code) != FALSE &&
                exit_code == 0) {
              tar_ok = true;
            }
          }
          CloseHandle(pi.hProcess);
          CloseHandle(pi.hThread);
        }
        DeleteFileW(zip_file.c_str());

        if (tar_ok) {
          const std::wstring flat_exe = tmp_dir + L"\\CrossPointer.exe";
          const std::wstring nested_exe =
              tmp_dir + L"\\CrossPointer\\CrossPointer.exe";
          const bool is_flat =
              (GetFileAttributesW(flat_exe.c_str()) != INVALID_FILE_ATTRIBUTES);
          const bool is_nested =
              (!is_flat && GetFileAttributesW(nested_exe.c_str()) !=
                               INVALID_FILE_ATTRIBUTES);

          if (is_flat || is_nested) {
            const std::wstring src_dir =
                is_flat ? (tmp_dir + L"\\") : (tmp_dir + L"\\CrossPointer\\");
            MoveFileExW((src_dir + L"CrossPointer.exe").c_str(),
                        new_exe.c_str(), MOVEFILE_REPLACE_EXISTING);
            MoveFileExW((src_dir + L"README.md").c_str(),
                        (dir + L"README.md").c_str(), MOVEFILE_REPLACE_EXISTING);
            MoveFileExW((src_dir + L"QuickStart_Guide.txt").c_str(),
                        (dir + L"QuickStart_Guide.txt").c_str(),
                        MOVEFILE_REPLACE_EXISTING);
            download_ok = true;
          }
        }
        RemoveDirectoryW((tmp_dir + L"\\CrossPointer").c_str());
        RemoveDirectoryW(tmp_dir.c_str());
      }
    }

    if (cancelled || cancel_flag.load()) {
      DeleteFileW(new_exe.c_str());
      DeleteFileW(zip_file.c_str());
      if (error_out) *error_out = L"已取消下载更新。";
      return false;
    }

    if (!download_ok ||
        GetFileAttributesW(new_exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
      if (error_out) *error_out = L"下载更新文件失败，请检查网络连接或代理设置。";
      return false;
    }

    if (on_progress) {
      DownloadProgress p;
      p.percent = 100;
      p.indeterminate = false;
      p.installing = true;
      p.stage_text = L"正在校验文件完整性并原地热替换...";
      on_progress(p);
    }

    HANDLE h_check = CreateFileW(new_exe.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                 nullptr, OPEN_EXISTING, 0, nullptr);
    if (h_check == INVALID_HANDLE_VALUE) {
      if (error_out) *error_out = L"无法读取下载的新版本可执行文件。";
      return false;
    }
    char mz[2] = {0, 0};
    DWORD read_bytes = 0;
    const BOOL read_ok = ReadFile(h_check, mz, 2, &read_bytes, nullptr);
    LARGE_INTEGER file_size = {};
    const BOOL size_ok = GetFileSizeEx(h_check, &file_size);
    CloseHandle(h_check);

    if (read_ok == FALSE || size_ok == FALSE || read_bytes < 2 ||
        mz[0] != 'M' || mz[1] != 'Z' || file_size.QuadPart < 100 * 1024) {
      DeleteFileW(new_exe.c_str());
      if (error_out) {
        *error_out = L"下载的新版本文件校验失败（非合法的 Windows 可执行程序）。";
      }
      return false;
    }

    DeleteFileW(old_exe.c_str());
    if (MoveFileExW(current_exe.c_str(), old_exe.c_str(),
                    MOVEFILE_REPLACE_EXISTING) == FALSE) {
      DeleteFileW(new_exe.c_str());
      if (error_out) {
        *error_out = L"无法重命名当前运行程序，可能被杀毒软件或文件系统锁定。";
      }
      return false;
    }

    if (MoveFileExW(new_exe.c_str(), current_exe.c_str(),
                    MOVEFILE_REPLACE_EXISTING) == FALSE) {
      MoveFileExW(old_exe.c_str(), current_exe.c_str(),
                  MOVEFILE_REPLACE_EXISTING);
      if (error_out) *error_out = L"无法将新版本写入目标路径。";
      return false;
    }

    const HINSTANCE h_inst =
        ShellExecuteW(nullptr, L"open", current_exe.c_str(),
                      L"--update-restart", dir.c_str(), SW_SHOW);
    if (reinterpret_cast<INT_PTR>(h_inst) <= 32) {
      MoveFileExW(old_exe.c_str(), current_exe.c_str(),
                  MOVEFILE_REPLACE_EXISTING);
      if (error_out) *error_out = L"新版本程序启动失败。";
      return false;
    }

    if (controller_hwnd != nullptr && IsWindow(controller_hwnd) != FALSE) {
      PostMessageW(controller_hwnd, WM_APP_REQUEST_EXIT, 0, 0);
    } else {
      ExitProcess(0);
    }
    return true;
  } catch (...) {
    if (error_out) *error_out = L"自动更新过程中发生异常。";
    return false;
  }
}

}  // namespace updater
