#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "./version.h"

namespace updater {

inline constexpr const wchar_t* kCurrentVersion = app_version::kVersionTag;

struct UpdateResult {
  bool success = false;
  bool has_update = false;
  std::wstring latest_version;
  std::wstring release_url;
  std::wstring changelog;
  std::wstring exe_download_url;
  std::wstring zip_download_url;
  std::wstring error_msg;
};

struct DownloadProgress {
  std::uint64_t downloaded_bytes = 0;
  std::uint64_t total_bytes = 0;
  double bytes_per_sec = 0.0;
  int percent = 0;
  bool indeterminate = true;
  bool installing = false;
  std::wstring stage_text;
};

using ProgressCallback = std::function<void(const DownloadProgress&)>;

[[nodiscard]] bool IsTrustedGitHubUrl(std::wstring_view url) noexcept;

// 同步检查更新（供工作线程调用）
[[nodiscard]] UpdateResult CheckUpdateSync() noexcept;

// 同步执行一键直接下载并重启更新（支持实时进度流式回调与取消标志）
[[nodiscard]] bool PerformDirectUpdate(const UpdateResult& update_info,
                                       HWND controller_hwnd,
                                       std::atomic<bool>& cancel_flag,
                                       ProgressCallback on_progress = nullptr,
                                       std::wstring* error_out = nullptr) noexcept;

// 启动时清理残留的旧版本临时备份文件 (.old.exe 等)
void CleanupOldExecutable() noexcept;

}  // namespace updater
