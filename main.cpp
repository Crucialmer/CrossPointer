#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cwchar>
#include <exception>
#include <memory>
#include <string>

#include "src/about_window.h"
#include "src/autostart.h"
#include "src/config.h"
#include "src/config_file_util.h"
#include "src/crosshair.h"
#include "src/hotkey.h"
#include "src/liquid_glass.h"
#include "src/settings_window.h"
#include "src/tray.h"
#include "src/tray_menu.h"
#include "src/update_window.h"
#include "src/updater.h"
#include "src/version.h"

namespace {
constexpr wchar_t kMutexName[] =
    L"Local\\CrossPointer_F5B6239126A64833BE094D6DC8DC1951";
constexpr wchar_t kControllerClassName[] = L"CrossPointerControllerClass";
constexpr UINT_PTR kTraySingleClickTimerId = 101;

enum class PreferredAppMode {
  Default,
  AllowDark,
  ForceDark,
  ForceLight,
  Max
};
using fnSetPreferredAppMode = PreferredAppMode(WINAPI*)(PreferredAppMode);
using fnFlushMenuThemes = void(WINAPI*)();

void EnableDarkModeSupport() noexcept {
  const HMODULE uxtheme = GetModuleHandleW(L"uxtheme.dll");
  const HMODULE mod =
      uxtheme != nullptr
          ? uxtheme
          : LoadLibraryExW(L"uxtheme.dll", nullptr,
                           LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (mod != nullptr) {
    const auto set_mode = reinterpret_cast<fnSetPreferredAppMode>(
        GetProcAddress(mod, MAKEINTRESOURCEA(135)));
    if (set_mode != nullptr) {
      set_mode(PreferredAppMode::AllowDark);
    }
    const auto flush_menus = reinterpret_cast<fnFlushMenuThemes>(
        GetProcAddress(mod, MAKEINTRESOURCEA(136)));
    if (flush_menus != nullptr) {
      flush_menus();
    }
  }
}

void RefreshMenuTheme() noexcept {
  if (const HMODULE mod = GetModuleHandleW(L"uxtheme.dll"); mod != nullptr) {
    const auto flush_menus = reinterpret_cast<fnFlushMenuThemes>(
        GetProcAddress(mod, MAKEINTRESOURCEA(136)));
    if (flush_menus != nullptr) {
      flush_menus();
    }
  }
}

class UniqueHandle {
 public:
  explicit UniqueHandle(HANDLE handle = nullptr) noexcept : handle_(handle) {}
  ~UniqueHandle() {
    if (handle_ != nullptr) CloseHandle(handle_);
  }
  UniqueHandle(const UniqueHandle&) = delete;
  UniqueHandle& operator=(const UniqueHandle&) = delete;

  UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.handle_) {
    other.handle_ = nullptr;
  }
  UniqueHandle& operator=(UniqueHandle&& other) noexcept {
    if (this != &other) {
      if (handle_ != nullptr) CloseHandle(handle_);
      handle_ = other.handle_;
      other.handle_ = nullptr;
    }
    return *this;
  }

  void reset(HANDLE h = nullptr) noexcept {
    if (handle_ != nullptr) CloseHandle(handle_);
    handle_ = h;
  }

  [[nodiscard]] HANDLE get() const noexcept { return handle_; }

 private:
  HANDLE handle_ = nullptr;
};

void EnableBestDpiAwareness() noexcept {
  using SetDpiContext = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
  if (const HMODULE user32 = GetModuleHandleW(L"user32.dll");
      user32 != nullptr) {
    const auto set_context = reinterpret_cast<SetDpiContext>(
        GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
    if (set_context != nullptr &&
        set_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE) {
      return;
    }
  }
  SetProcessDPIAware();
}

std::wstring FormatSingleHotkey(const HotkeyConfig& hotkey) {
  std::wstring s;
  if ((hotkey.mod & MOD_CONTROL) != 0) s += L"Ctrl+";
  if ((hotkey.mod & MOD_ALT) != 0) s += L"Alt+";
  if ((hotkey.mod & MOD_WIN) != 0) s += L"Win+";
  if ((hotkey.mod & MOD_SHIFT) != 0) s += L"Shift+";
  s += Config::VKToString(hotkey.vk);
  return s;
}

std::wstring FormatHotkeyTooltip(const HotkeyConfig& toggle,
                                 const HotkeyConfig& exit) {
  if (toggle.mod == exit.mod && toggle.vk == exit.vk) {
    return L"CrossPointer (单键复用: " + FormatSingleHotkey(toggle) +
           L" 显示/退出)";
  }
  return L"CrossPointer (显隐: " + FormatSingleHotkey(toggle) + L", 退出: " +
         FormatSingleHotkey(exit) + L")";
}

struct AppContext {
  HINSTANCE instance = nullptr;
  UINT taskbar_created_msg = 0;
  bool ignore_next_tray_lbutton_up = false;
  std::wstring config_path;
  Config raw_config;
  Config active_config;
  CrosshairWindow* crosshair = nullptr;
  TrayManager tray;
  TrayMenuWindow tray_menu;
  SettingsWindow settings_win;
  AboutWindow about_win;
  SoftwareUpdateWindow update_win;

  void RefreshActiveTheme() {
    const bool is_light = IsSystemInLightTheme();
    active_config = GetThemedConfig(raw_config, is_light);
    if (crosshair != nullptr) {
      (void)crosshair->ApplyConfig(active_config);
    }
  }

  enum class ReloadStatus { kOk, kFileError, kHotkeyConflict };
  ReloadStatus TryReloadConfig() {
    Config updated;
    if (!updated.Load(config_path.c_str())) {
      return ReloadStatus::kFileError;
    }
    if (!HotkeyManager::Register(updated.hotkey_h_s, updated.hotkey_exit)) {
      (void)HotkeyManager::Register(raw_config.hotkey_h_s,
                                    raw_config.hotkey_exit);
      return ReloadStatus::kHotkeyConflict;
    }
    raw_config = updated;
    RefreshActiveTheme();
    tray.UpdateTooltip(
        FormatHotkeyTooltip(raw_config.hotkey_h_s, raw_config.hotkey_exit));
    return ReloadStatus::kOk;
  }
};

LRESULT CALLBACK ControllerWndProc(HWND window, UINT message, WPARAM w_param,
                                   LPARAM l_param) noexcept {
  auto* ctx =
      reinterpret_cast<AppContext*>(GetWindowLongPtrW(window, GWLP_USERDATA));

  if (ctx != nullptr && ctx->taskbar_created_msg != 0 &&
      message == ctx->taskbar_created_msg) {
    ctx->tray.ReAdd();
    return 0;
  }

  switch (message) {
    case WM_NCCREATE: {
      const auto* create = reinterpret_cast<const CREATESTRUCTW*>(l_param);
      ctx = static_cast<AppContext*>(create->lpCreateParams);
      SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
      return DefWindowProcW(window, message, w_param, l_param);
    }

    case WM_SETTINGCHANGE: {
      if (ctx != nullptr) {
        const auto* param = reinterpret_cast<const wchar_t*>(l_param);
        if (param == nullptr || wcscmp(param, L"ImmersiveColorSet") == 0) {
          RefreshMenuTheme();
          if (ctx->raw_config.auto_theme) {
            ctx->RefreshActiveTheme();
          }
          ctx->settings_win.NotifyThemeChanged();
          ctx->about_win.NotifyThemeChanged();
          ctx->update_win.NotifyThemeChanged();
        }
      }
      break;
    }

    case WM_TIMER: {
      if (ctx != nullptr && w_param == kTraySingleClickTimerId) {
        KillTimer(window, kTraySingleClickTimerId);
        PostMessageW(window, WM_COMMAND, IDM_TRAY_TOGGLE, 0);
        return 0;
      }
      break;
    }

    case WM_TRAYICON: {
      if (ctx == nullptr || ctx->crosshair == nullptr) break;
      if (l_param == WM_RBUTTONUP) {
        POINT pt = {};
        GetCursorPos(&pt);
        ctx->tray_menu.Show(ctx->instance, window, pt,
                            ctx->crosshair->IsVisible(),
                            FormatSingleHotkey(ctx->raw_config.hotkey_h_s));
      } else if (l_param == WM_LBUTTONUP) {
        if (ctx->ignore_next_tray_lbutton_up) {
          ctx->ignore_next_tray_lbutton_up = false;
        } else {
          const UINT delay = std::min<UINT>(GetDoubleClickTime(), 250U);
          SetTimer(window, kTraySingleClickTimerId, delay, nullptr);
        }
      } else if (l_param == WM_LBUTTONDBLCLK) {
        KillTimer(window, kTraySingleClickTimerId);
        ctx->ignore_next_tray_lbutton_up = true;
        ctx->tray_menu.Close();
        PostMessageW(window, WM_COMMAND, IDM_TRAY_SETTINGS, 0);
      }
      return 0;
    }

    case WM_APP_SILENT_UPDATE_FOUND: {
      std::unique_ptr<updater::UpdateResult> res(
          reinterpret_cast<updater::UpdateResult*>(l_param));
      if (ctx != nullptr && res && res->has_update) {
        ctx->update_win.ShowWithResult(ctx->instance, window, *res);
      }
      return 0;
    }

    case WM_APP_REQUEST_EXIT: {
      PostQuitMessage(0);
      return 0;
    }

    case WM_COMMAND: {
      if (ctx == nullptr || ctx->crosshair == nullptr) break;
      const UINT command_id = LOWORD(w_param);
      switch (command_id) {
        case IDM_TRAY_TOGGLE: {
          ctx->crosshair->ToggleVisible();
          ctx->settings_win.NotifyVisibilityChanged(
              ctx->crosshair->IsVisible());
          return 0;
        }

        case IDM_TRAY_SETTINGS: {
          ctx->settings_win.Show(
              ctx->instance, window, ctx->raw_config, ctx->config_path,
              ctx->crosshair->IsVisible(),
              [ctx](const Config& new_config) -> bool {
                if (!HotkeyManager::Register(new_config.hotkey_h_s,
                                             new_config.hotkey_exit)) {
                  (void)HotkeyManager::Register(ctx->raw_config.hotkey_h_s,
                                                ctx->raw_config.hotkey_exit);
                  return false;
                }
                ctx->raw_config = new_config;
                ctx->RefreshActiveTheme();
                ctx->tray.UpdateTooltip(FormatHotkeyTooltip(
                    ctx->raw_config.hotkey_h_s, ctx->raw_config.hotkey_exit));
                return true;
              });
          return 0;
        }

        case IDM_TRAY_CHECK_UPDATE: {
          ctx->update_win.ShowAndCheck(ctx->instance, window);
          return 0;
        }

        case IDM_TRAY_OPEN_CONFIG: {
          ShellExecuteW(nullptr, L"open", ctx->config_path.c_str(), nullptr,
                        nullptr, SW_SHOW);
          return 0;
        }

        case IDM_TRAY_RELOAD: {
          const auto status = ctx->TryReloadConfig();
          if (status == AppContext::ReloadStatus::kFileError) {
            liquid_glass::ShowAlert(
                window, L"配置读取失败",
                L"CrossPointer.ini 格式存在错误，请修正后重试。",
                liquid_glass::OrbKind::kWarning);
          } else if (status == AppContext::ReloadStatus::kHotkeyConflict) {
            liquid_glass::ShowAlert(
                window, L"热键冲突",
                L"新配置中的快捷键已被其他程序占用，已自动保留原有配置。",
                liquid_glass::OrbKind::kWarning);
          } else {
            liquid_glass::ShowAlert(
                window, L"配置已重新载入",
                L"CrossPointer.ini 已成功重新载入并即时生效。",
                liquid_glass::OrbKind::kSuccess);
          }
          return 0;
        }

        case IDM_TRAY_ABOUT: {
          ctx->about_win.Show(ctx->instance, window);
          return 0;
        }

        case IDM_TRAY_EXIT: {
          PostQuitMessage(0);
          return 0;
        }
      }
      break;
    }

    case WM_DESTROY: {
      KillTimer(window, kTraySingleClickTimerId);
      if (ctx != nullptr) {
        ctx->tray_menu.Close();
        ctx->update_win.Close();
        ctx->about_win.Close();
        ctx->settings_win.Close();
        ctx->tray.Remove();
      }
      break;
    }
  }
  return DefWindowProcW(window, message, w_param, l_param);
}

int Run(HINSTANCE instance) {
  updater::CleanupOldExecutable();

  const bool is_update_restart =
      (wcsstr(GetCommandLineW(), L"--update-restart") != nullptr);
  UniqueHandle mutex;
  const int max_tries = is_update_restart ? 30 : 1;
  for (int i = 0; i < max_tries; ++i) {
    HANDLE h = CreateMutexW(nullptr, FALSE, kMutexName);
    if (h != nullptr && GetLastError() != ERROR_ALREADY_EXISTS) {
      mutex.reset(h);
      break;
    }
    if (h != nullptr) CloseHandle(h);
    if (!is_update_restart) break;
    Sleep(100);
  }

  EnableDarkModeSupport();
  EnableBestDpiAwareness();

  if (mutex.get() == nullptr) {
    if (!is_update_restart) {
      liquid_glass::ShowAlert(
          nullptr, L"CrossPointer 已在运行",
          L"程序已在后台静默运行，可在右下角系统托盘图标中右键打开菜单或双击打开偏好设置。",
          liquid_glass::OrbKind::kInfo);
    }
    return 0;
  }

  const std::wstring config_path = GetConfigPath();
  if (!EnsureConfigExists(config_path)) return 1;

  Config raw_config;
  if (!raw_config.Load(config_path.c_str())) {
    liquid_glass::ShowAlert(
        nullptr, L"配置文件无效",
        L"CrossPointer.ini 配置格式异常，请修正或删除该文件后重新启动程序。",
        liquid_glass::OrbKind::kError);
    return 1;
  }

  Config active_config = GetThemedConfig(raw_config, IsSystemInLightTheme());

  CrosshairWindow crosshair(instance, active_config);
  if (!crosshair.Create(raw_config.show_on_startup)) {
    liquid_glass::ShowAlert(nullptr, L"初始化失败",
                            L"创建或初始化 Direct2D 十字辅助线窗口失败。",
                            liquid_glass::OrbKind::kError);
    return 1;
  }
  if (!HotkeyManager::Register(raw_config.hotkey_h_s, raw_config.hotkey_exit)) {
    liquid_glass::ShowAlert(
        nullptr, L"全局热键冲突",
        L"注册全局快捷键失败，热键可能已被其他程序占用。请修改 CrossPointer.ini 后重试。",
        liquid_glass::OrbKind::kWarning);
    return 1;
  }

  AppContext context;
  context.instance = instance;
  context.taskbar_created_msg = RegisterWindowMessageW(L"TaskbarCreated");
  context.config_path = config_path;
  context.raw_config = raw_config;
  context.active_config = active_config;
  context.crosshair = &crosshair;

  WNDCLASSEXW wc = {sizeof(wc)};
  wc.lpfnWndProc = ControllerWndProc;
  wc.hInstance = instance;
  wc.lpszClassName = kControllerClassName;
  if (RegisterClassExW(&wc) == 0) return 1;

  HWND controller = CreateWindowExW(
      0, kControllerClassName, L"CrossPointerController", WS_OVERLAPPED, 0, 0,
      0, 0, nullptr, nullptr, instance, &context);
  if (controller == nullptr) {
    UnregisterClassW(kControllerClassName, instance);
    return 1;
  }

  (void)context.tray.Initialize(instance, controller);
  if (is_update_restart) {
    context.tray.UpdateTooltip(
        std::wstring(L"CrossPointer ") + app_version::kVersionTag +
        L" (已成功更新至最新版本)");
  } else {
    context.tray.UpdateTooltip(FormatHotkeyTooltip(
        context.raw_config.hotkey_h_s, context.raw_config.hotkey_exit));
  }

  if (context.raw_config.auto_check_update) {
    SoftwareUpdateWindow::StartSilentCheck(controller,
                                           WM_APP_SILENT_UPDATE_FOUND);
  }

  int result = 0;
  MSG message = {};
  while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
    if (message.message == WM_HOTKEY) {
      if (message.wParam == kExitHotkeyId) {
        PostQuitMessage(0);
        continue;
      }
      if (message.wParam == kToggleHotkeyId) {
        const bool is_same_hotkey =
            (context.raw_config.hotkey_h_s.mod ==
                 context.raw_config.hotkey_exit.mod &&
             context.raw_config.hotkey_h_s.vk ==
                 context.raw_config.hotkey_exit.vk);

        if (crosshair.IsVisible()) {
          if (is_same_hotkey) {
            PostQuitMessage(0);
          } else {
            crosshair.ToggleVisible();
            context.settings_win.NotifyVisibilityChanged(crosshair.IsVisible());
          }
          continue;
        }

        (void)context.TryReloadConfig();
        crosshair.ToggleVisible();
        context.settings_win.NotifyVisibilityChanged(crosshair.IsVisible());
        continue;
      }
    }

    if (context.settings_win.IsOpen() &&
        IsDialogMessageW(context.settings_win.GetHwnd(), &message) != FALSE) {
      continue;
    }

    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  HotkeyManager::UnregisterAll();
  context.tray_menu.Close();
  context.update_win.Close();
  context.about_win.Close();
  context.settings_win.Close();
  context.tray.Remove();
  DestroyWindow(controller);
  UnregisterClassW(kControllerClassName, instance);

  if (result == -1) {
    return 1;
  }
  return static_cast<int>(message.wParam);
}
}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  try {
    return Run(instance);
  } catch (const std::exception&) {
    MessageBoxW(nullptr, L"程序发生未处理的 C++ 异常。", L"严重错误",
                MB_OK | MB_ICONERROR);
  } catch (...) {
    MessageBoxW(nullptr, L"程序发生未知异常。", L"严重错误",
                MB_OK | MB_ICONERROR);
  }
  return 1;
}
