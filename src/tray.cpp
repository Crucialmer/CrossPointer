#include "./tray.h"

#include <shellapi.h>

namespace {
constexpr UINT kTrayId = 1001;

int GetTrayIconSize() noexcept {
  using GetSystemMetricsForDpiFn = int(WINAPI*)(int, UINT);
  using GetDpiForSystemFn = UINT(WINAPI*)();
  if (const HMODULE user32 = GetModuleHandleW(L"user32.dll"); user32 != nullptr) {
    const auto metrics_dpi = reinterpret_cast<GetSystemMetricsForDpiFn>(
        GetProcAddress(user32, "GetSystemMetricsForDpi"));
    const auto sys_dpi = reinterpret_cast<GetDpiForSystemFn>(
        GetProcAddress(user32, "GetDpiForSystem"));
    if (metrics_dpi != nullptr && sys_dpi != nullptr) {
      const int size = metrics_dpi(SM_CXSMICON, sys_dpi());
      if (size > 0) return size;
    }
  }
  const int size = GetSystemMetrics(SM_CXSMICON);
  return size > 0 ? size : 16;
}
}  // namespace

TrayManager::~TrayManager() { Remove(); }

bool TrayManager::Initialize(HINSTANCE instance, HWND window) {
  window_ = window;
  const int icon_size = GetTrayIconSize();
  icon_ = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(101),
                                        IMAGE_ICON, icon_size, icon_size,
                                        LR_DEFAULTCOLOR));
  if (icon_ != nullptr) {
    owns_icon_ = true;
  } else {
    icon_ = LoadIconW(nullptr, IDI_APPLICATION);
    owns_icon_ = false;
  }

  NOTIFYICONDATAW nid = {sizeof(nid)};
  nid.hWnd = window_;
  nid.uID = kTrayId;
  nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  nid.uCallbackMessage = WM_TRAYICON;
  nid.hIcon = icon_;
  wcsncpy_s(nid.szTip, last_tooltip_.c_str(), _countof(nid.szTip) - 1);

  added_ = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
  return added_;
}

void TrayManager::ReAdd() noexcept {
  if (window_ == nullptr || icon_ == nullptr) return;
  NOTIFYICONDATAW nid = {sizeof(nid)};
  nid.hWnd = window_;
  nid.uID = kTrayId;
  nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  nid.uCallbackMessage = WM_TRAYICON;
  nid.hIcon = icon_;
  wcsncpy_s(nid.szTip, last_tooltip_.c_str(), _countof(nid.szTip) - 1);
  added_ = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
}

void TrayManager::Remove() noexcept {
  if (added_ && window_ != nullptr) {
    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = window_;
    nid.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    added_ = false;
  }
  if (icon_ != nullptr) {
    if (owns_icon_) {
      DestroyIcon(icon_);
    }
    icon_ = nullptr;
    owns_icon_ = false;
  }
}

void TrayManager::UpdateTooltip(const std::wstring& text) noexcept {
  last_tooltip_ = text;
  if (!added_ || window_ == nullptr) return;
  NOTIFYICONDATAW nid = {sizeof(nid)};
  nid.hWnd = window_;
  nid.uID = kTrayId;
  nid.uFlags = NIF_TIP;
  wcsncpy_s(nid.szTip, text.c_str(), _countof(nid.szTip) - 1);
  Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayManager::ShowContextMenu(HWND window, POINT pt,
                                  bool is_visible) const noexcept {
  HMENU menu = CreatePopupMenu();
  if (menu == nullptr) return;

  const wchar_t* toggle_text =
      is_visible ? L"隐藏十字辅助线 (&H)" : L"显示十字辅助线 (&S)";
  AppendMenuW(menu, MF_STRING, IDM_TRAY_TOGGLE, toggle_text);
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, IDM_TRAY_SETTINGS, L"偏好设置... (&P)");
  AppendMenuW(menu, MF_STRING, IDM_TRAY_CHECK_UPDATE, L"检查更新... (&U)");
  AppendMenuW(menu, MF_STRING, IDM_TRAY_RELOAD, L"重新载入配置 (&R)");
  AppendMenuW(menu, MF_STRING, IDM_TRAY_OPEN_CONFIG, L"打开配置文件 (&O)");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, IDM_TRAY_ABOUT, L"关于 CrossPointer (&A)");
  AppendMenuW(menu, MF_STRING, IDM_TRAY_EXIT, L"退出 (&X)");

  // 突出显示默认双击项「偏好设置」
  SetMenuDefaultItem(menu, IDM_TRAY_SETTINGS, FALSE);

  SetForegroundWindow(window);
  TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, window, nullptr);
  PostMessageW(window, WM_NULL, 0, 0);
  DestroyMenu(menu);
}
