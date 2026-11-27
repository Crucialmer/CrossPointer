#include "./crosshair.h"

#include <dwmapi.h>
#include <timeapi.h>

#include <algorithm>
#include <cmath>

#ifndef DWMWA_TRANSITIONS_FORCEDISABLED
#define DWMWA_TRANSITIONS_FORCEDISABLED 3
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_DONOTROUND
#define DWMWCP_DONOTROUND 1
#endif

namespace {
constexpr wchar_t kClassName[] = L"MouseCrosshairWindow";
constexpr DWORD kExtendedWindowStyle = WS_EX_LAYERED | WS_EX_TRANSPARENT |
                                       WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
                                       WS_EX_NOACTIVATE;

template <typename T>
void SafeRelease(T*& object) noexcept {
  if (object != nullptr) {
    object->Release();
    object = nullptr;
  }
}

D2D1_COLOR_F ToD2DColor(const LineConfig& line) noexcept {
  return D2D1::ColorF(static_cast<float>(line.r.value) / 255.0F,
                      static_cast<float>(line.g.value) / 255.0F,
                      static_cast<float>(line.b.value) / 255.0F, 1.0F);
}

float GetMonitorDpiScale(HMONITOR monitor) noexcept {
  using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
  static const auto get_dpi_fn = []() -> GetDpiForMonitorFn {
    const HMODULE shcore = LoadLibraryExW(L"shcore.dll", nullptr,
                                          LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (shcore != nullptr) {
      return reinterpret_cast<GetDpiForMonitorFn>(
          GetProcAddress(shcore, "GetDpiForMonitor"));
    }
    return nullptr;
  }();

  if (get_dpi_fn != nullptr && monitor != nullptr) {
    UINT dpi_x = 96;
    UINT dpi_y = 96;
    if (SUCCEEDED(get_dpi_fn(monitor, 0 /* MDT_EFFECTIVE_DPI */, &dpi_x,
                             &dpi_y)) &&
        dpi_x > 0) {
      return static_cast<float>(dpi_x) / 96.0f;
    }
  }
  return 1.0f;
}

void ConfigureOverlayDwm(HWND hwnd) noexcept {
  if (hwnd == nullptr) return;
  const BOOL disable_transitions = TRUE;
  DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED,
                        &disable_transitions, sizeof(disable_transitions));
  const DWORD corner_pref = DWMWCP_DONOTROUND;
  DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner_pref,
                        sizeof(corner_pref));
}
}  // namespace

CrosshairWindow::CrosshairWindow(HINSTANCE instance,
                                 const Config& config) noexcept
    : instance_(instance), config_(config) {}

CrosshairWindow::~CrosshairWindow() {
  SetTrackingActive(false);
  SetWindowsVisible(false);
  CleanupDirect2D();
  DestroyWindows();
  if (class_registered_) {
    UnregisterClassW(kClassName, instance_);
  }
}

void CrosshairWindow::DestroyWindows() noexcept {
  for (auto& arm : arms_) {
    if (arm.hwnd != nullptr) {
      SetWindowLongPtrW(arm.hwnd, GWLP_USERDATA, 0);
      DestroyWindow(arm.hwnd);
      arm.hwnd = nullptr;
      arm.shown = false;
      arm.last_rect = {0, 0, 0, 0};
    }
  }
}

void CrosshairWindow::SetTrackingActive(bool active) noexcept {
  if (active) {
    if (!precision_timer_active_) {
      timeBeginPeriod(1);
      precision_timer_active_ = true;
    }
    if (!timer_running_ && arms_[0].hwnd != nullptr) {
      timer_running_ =
          SetTimer(arms_[0].hwnd, kUpdateTimerId, kUpdateIntervalMs, nullptr) != 0;
    }
  } else {
    if (timer_running_ && arms_[0].hwnd != nullptr) {
      KillTimer(arms_[0].hwnd, kUpdateTimerId);
      timer_running_ = false;
    }
    if (precision_timer_active_) {
      timeEndPeriod(1);
      precision_timer_active_ = false;
    }
  }
}

void CrosshairWindow::InvalidateMonitorCache() noexcept {
  cached_monitor_ = nullptr;
  last_cursor_pos_ = {LONG_MIN, LONG_MIN};
}

bool CrosshairWindow::Create(bool initial_visible) {
  WNDCLASSEXW window_class = {sizeof(window_class)};
  window_class.lpfnWndProc = WndProc;
  window_class.hInstance = instance_;
  window_class.lpszClassName = kClassName;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);

  if (RegisterClassExW(&window_class) == 0) {
    return false;
  }
  class_registered_ = true;

  for (auto& arm : arms_) {
    arm.hwnd = CreateWindowExW(kExtendedWindowStyle, kClassName, L"", WS_POPUP,
                               0, 0, 1, 1, nullptr, nullptr, instance_, this);
    if (arm.hwnd == nullptr) {
      DestroyWindows();
      UnregisterClassW(kClassName, instance_);
      class_registered_ = false;
      return false;
    }
    ConfigureOverlayDwm(arm.hwnd);
  }

  visible_ = false;
  if (!ApplyConfig(config_) || !InitializeDirect2D()) {
    CleanupDirect2D();
    DestroyWindows();
    UnregisterClassW(kClassName, instance_);
    class_registered_ = false;
    return false;
  }

  if (initial_visible) {
    SetVisible(true);
  }
  return true;
}

bool CrosshairWindow::InitializeDirect2D() {
  CleanupDirect2D();
  for (const auto& arm : arms_) {
    if (arm.hwnd == nullptr) return false;
  }

  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                               &d2d_factory_))) {
    return false;
  }

  const D2D1_RENDER_TARGET_PROPERTIES target_properties =
      D2D1::RenderTargetProperties(
          D2D1_RENDER_TARGET_TYPE_DEFAULT,
          D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                            D2D1_ALPHA_MODE_PREMULTIPLIED));

  for (auto& arm : arms_) {
    RECT client = {};
    if (!GetClientRect(arm.hwnd, &client)) {
      CleanupDirect2D();
      return false;
    }
    const UINT width = static_cast<UINT>(std::max(1L, client.right - client.left));
    const UINT height = static_cast<UINT>(std::max(1L, client.bottom - client.top));
    // 使用 D2D1_PRESENT_OPTIONS_IMMEDIATELY 避免 4 个窗口串联阻塞等待 VSync
    const auto hwnd_props = D2D1::HwndRenderTargetProperties(
        arm.hwnd, D2D1::SizeU(width, height), D2D1_PRESENT_OPTIONS_IMMEDIATELY);
    if (FAILED(d2d_factory_->CreateHwndRenderTarget(
            target_properties, hwnd_props, &arm.target))) {
      CleanupDirect2D();
      return false;
    }
    arm.target->SetDpi(96.0F, 96.0F);
  }
  return true;
}

void CrosshairWindow::CleanupDirect2D() noexcept {
  for (auto& arm : arms_) {
    SafeRelease(arm.target);
  }
  SafeRelease(d2d_factory_);
}

bool CrosshairWindow::ApplyConfig(const Config& config) {
  config_ = config;
  if (arms_[0].hwnd == nullptr) {
    return true;
  }

  const BYTE h_alpha = config_.horizontal.alpha.value;
  const BYTE v_alpha = config_.vertical.alpha.value;
  if (!SetLayeredWindowAttributes(arms_[0].hwnd, 0, h_alpha, LWA_ALPHA) ||
      !SetLayeredWindowAttributes(arms_[1].hwnd, 0, h_alpha, LWA_ALPHA) ||
      !SetLayeredWindowAttributes(arms_[2].hwnd, 0, v_alpha, LWA_ALPHA) ||
      !SetLayeredWindowAttributes(arms_[3].hwnd, 0, v_alpha, LWA_ALPHA)) {
    return false;
  }

  InvalidateMonitorCache();
  if (visible_) {
    UpdatePosition();
    for (const auto& arm : arms_) {
      if (arm.shown) {
        InvalidateRect(arm.hwnd, nullptr, FALSE);
      }
    }
  }
  return true;
}

void CrosshairWindow::ToggleVisible() { SetVisible(!visible_); }

void CrosshairWindow::SetVisible(bool visible) noexcept {
  if (visible_ == visible) return;
  visible_ = visible;
  InvalidateMonitorCache();
  if (visible_) {
    SetTrackingActive(true);
    UpdatePosition();
  } else {
    SetTrackingActive(false);
    SetWindowsVisible(false);
  }
}

void CrosshairWindow::SetWindowsVisible(bool visible) noexcept {
  for (auto& arm : arms_) {
    if (arm.hwnd != nullptr) {
      ShowWindow(arm.hwnd, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
      arm.shown = visible;
      if (!visible) {
        arm.last_rect = {0, 0, 0, 0};
      }
    }
  }
}

void CrosshairWindow::UpdatePosition() {
  if (!visible_ || arms_[0].hwnd == nullptr) {
    return;
  }

  POINT cursor = {};
  if (!GetCursorPos(&cursor)) {
    return;
  }
  if (cursor.x == last_cursor_pos_.x && cursor.y == last_cursor_pos_.y) {
    return;
  }
  last_cursor_pos_ = cursor;

  const HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONULL);
  if (monitor == nullptr) {
    SetWindowsVisible(false);
    return;
  }

  if (monitor != cached_monitor_) {
    MONITORINFO monitor_info = {sizeof(monitor_info)};
    if (!GetMonitorInfoW(monitor, &monitor_info)) {
      SetWindowsVisible(false);
      return;
    }
    cached_monitor_ = monitor;
    cached_bounds_ = monitor_info.rcMonitor;
    cached_dpi_scale_ = config_.auto_dpi ? GetMonitorDpiScale(monitor) : 1.0f;
  }

  const RECT& bounds = cached_bounds_;
  const float dpi_scale = cached_dpi_scale_;

  const int gap = static_cast<int>(std::round(config_.gap * dpi_scale));
  const int horizontal_width = std::max(
      1, static_cast<int>(std::round(config_.horizontal.width * dpi_scale)));
  const int vertical_width = std::max(
      1, static_cast<int>(std::round(config_.vertical.width * dpi_scale)));

  const int max_h_y = std::max<int>(bounds.top, bounds.bottom - horizontal_width);
  const int max_v_x = std::max<int>(bounds.left, bounds.right - vertical_width);
  const int horizontal_y =
      std::clamp(static_cast<int>(cursor.y) - horizontal_width / 2,
                 static_cast<int>(bounds.top), max_h_y);
  const int vertical_x =
      std::clamp(static_cast<int>(cursor.x) - vertical_width / 2,
                 static_cast<int>(bounds.left), max_v_x);
  const int horizontal_height = std::min(
      horizontal_width, static_cast<int>(bounds.bottom - horizontal_y));
  const int vertical_size =
      std::min(vertical_width, static_cast<int>(bounds.right - vertical_x));

  const int left_end = std::clamp(static_cast<int>(cursor.x) - gap,
                                  static_cast<int>(bounds.left),
                                  static_cast<int>(bounds.right));
  const int right_start = std::clamp(static_cast<int>(cursor.x) + gap,
                                     static_cast<int>(bounds.left),
                                     static_cast<int>(bounds.right));
  const int top_end = std::clamp(static_cast<int>(cursor.y) - gap,
                                 static_cast<int>(bounds.top),
                                 static_cast<int>(bounds.bottom));
  const int bottom_start = std::clamp(static_cast<int>(cursor.y) + gap,
                                      static_cast<int>(bounds.top),
                                      static_cast<int>(bounds.bottom));

  const std::array<RECT, 4> target_rects = {{
      {bounds.left, horizontal_y, left_end, horizontal_y + horizontal_height},
      {right_start, horizontal_y, bounds.right, horizontal_y + horizontal_height},
      {vertical_x, bounds.top, vertical_x + vertical_size, top_end},
      {vertical_x, bottom_start, vertical_x + vertical_size, bounds.bottom},
  }};

  std::array<bool, 4> needs_repaint = {false, false, false, false};

  HDWP positions = BeginDeferWindowPos(4);
  if (positions == nullptr) {
    return;
  }

  for (size_t i = 0; i < 4; ++i) {
    auto& arm = arms_[i];
    const RECT& rc = target_rects[i];
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;

    if (w <= 0 || h <= 0) {
      if (arm.shown) {
        positions = DeferWindowPos(
            positions, arm.hwnd, nullptr, 0, 0, 0, 0,
            SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                SWP_HIDEWINDOW);
        arm.shown = false;
        arm.last_rect = {0, 0, 0, 0};
      }
      if (positions == nullptr) break;
      continue;
    }

    const int prev_w = arm.last_rect.right - arm.last_rect.left;
    const int prev_h = arm.last_rect.bottom - arm.last_rect.top;
    const bool size_changed = (w != prev_w || h != prev_h);
    const bool newly_shown = !arm.shown;

    UINT flags = SWP_NOACTIVATE | SWP_SHOWWINDOW;
    if (!size_changed && !newly_shown) {
      flags |= SWP_NOREDRAW;
    }

    positions = DeferWindowPos(positions, arm.hwnd, HWND_TOPMOST, rc.left,
                               rc.top, w, h, flags);
    if (positions == nullptr) break;

    if (size_changed || newly_shown) {
      needs_repaint[i] = true;
    }
    arm.last_rect = rc;
    arm.shown = true;
  }

  if (positions == nullptr || !EndDeferWindowPos(positions)) {
    SetWindowsVisible(false);
    last_cursor_pos_ = {LONG_MIN, LONG_MIN};
    return;
  }

  for (size_t i = 0; i < 4; ++i) {
    if (needs_repaint[i]) {
      InvalidateRect(arms_[i].hwnd, nullptr, FALSE);
    }
  }
}

HRESULT CrosshairWindow::RenderArm(size_t index) const noexcept {
  ID2D1HwndRenderTarget* target = arms_[index].target;
  if (target == nullptr) {
    return E_INVALIDARG;
  }
  const LineConfig& line = (index < 2) ? config_.horizontal : config_.vertical;
  target->BeginDraw();
  target->Clear(ToD2DColor(line));
  return target->EndDraw();
}

LRESULT CALLBACK CrosshairWindow::WndProc(HWND window, UINT message,
                                          WPARAM w_param,
                                          LPARAM l_param) noexcept {
  CrosshairWindow* self = reinterpret_cast<CrosshairWindow*>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<const CREATESTRUCTW*>(l_param);
    self = static_cast<CrosshairWindow*>(create->lpCreateParams);
    SetLastError(ERROR_SUCCESS);
    if (SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(self)) == 0 &&
        GetLastError() != ERROR_SUCCESS) {
      return FALSE;
    }
  }

  switch (message) {
    case WM_TIMER:
      if (self != nullptr && w_param == kUpdateTimerId &&
          window == self->arms_[0].hwnd) {
        self->UpdatePosition();
        return 0;
      }
      break;
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
      if (self != nullptr && window == self->arms_[0].hwnd) {
        self->InvalidateMonitorCache();
        if (self->visible_) {
          self->UpdatePosition();
        }
        return 0;
      }
      break;
    case WM_PAINT: {
      PAINTSTRUCT paint = {};
      if (BeginPaint(window, &paint) == nullptr) {
        return 0;
      }
      if (self != nullptr) {
        for (size_t i = 0; i < self->arms_.size(); ++i) {
          if (window != self->arms_[i].hwnd) continue;
          ID2D1HwndRenderTarget* target = self->arms_[i].target;
          if (target != nullptr) {
            RECT client = {};
            HRESULT result = E_FAIL;
            if (GetClientRect(window, &client)) {
              const UINT width =
                  static_cast<UINT>(std::max(1L, client.right - client.left));
              const UINT height =
                  static_cast<UINT>(std::max(1L, client.bottom - client.top));
              const D2D1_SIZE_U cur_size = target->GetPixelSize();
              result = S_OK;
              if (cur_size.width != width || cur_size.height != height) {
                result = target->Resize(D2D1::SizeU(width, height));
              }
              if (SUCCEEDED(result)) {
                result = self->RenderArm(i);
              }
            }
            if (result == D2DERR_RECREATE_TARGET &&
                !self->InitializeDirect2D()) {
              self->SetWindowsVisible(false);
            }
          }
          break;
        }
      }
      EndPaint(window, &paint);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_NCDESTROY:
      SetWindowLongPtrW(window, GWLP_USERDATA, 0);
      break;
    default:
      break;
  }
  return DefWindowProcW(window, message, w_param, l_param);
}
