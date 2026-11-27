#include "./autostart.h"

#include <string>

#include "./config_file_util.h"

namespace autostart {
namespace {
constexpr wchar_t kRunSubKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kAppName[] = L"CrossPointer";
}  // namespace

bool IsAutoStartEnabled() noexcept {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunSubKey, 0, KEY_QUERY_VALUE, &key) !=
      ERROR_SUCCESS) {
    return false;
  }

  DWORD type = 0;
  DWORD size = 0;
  const LSTATUS status =
      RegQueryValueExW(key, kAppName, nullptr, &type, nullptr, &size);
  RegCloseKey(key);
  return (status == ERROR_SUCCESS &&
          (type == REG_SZ || type == REG_EXPAND_SZ) && size > sizeof(wchar_t));
}

bool SetAutoStart(bool enable) noexcept {
  if (enable) {
    const std::wstring module_path = GetExecutablePath();
    if (module_path.empty()) return false;

    const std::wstring cmd = L"\"" + module_path + L"\"";
    const LSTATUS status = RegSetKeyValueW(
        HKEY_CURRENT_USER, kRunSubKey, kAppName, REG_SZ, cmd.c_str(),
        static_cast<DWORD>((cmd.length() + 1) * sizeof(wchar_t)));
    return status == ERROR_SUCCESS;
  }

  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunSubKey, 0, KEY_SET_VALUE, &key) !=
      ERROR_SUCCESS) {
    return true;
  }
  const LSTATUS status = RegDeleteValueW(key, kAppName);
  RegCloseKey(key);
  return (status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND);
}

}  // namespace autostart
