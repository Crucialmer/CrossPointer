#pragma once

#include <d2d1.h>
#include <windows.h>

#include <array>

#include "config.h"

#pragma comment(lib, "d2d1.lib")

class CrosshairWindow {
 public:
  CrosshairWindow(HINSTANCE instance, const Config& config) noexcept;
  ~CrosshairWindow();

  CrosshairWindow(const CrosshairWindow&) = delete;
  CrosshairWindow& operator=(const CrosshairWindow&) = delete;

  [[nodiscard]] bool Create(bool initial_visible = true);
  void ToggleVisible();
  void SetVisible(bool visible) noexcept;
  [[nodiscard]] bool IsVisible() const noexcept { return visible_; }
  [[nodiscard]] bool ApplyConfig(const Config& config);

 private:
  static constexpr UINT_PTR kUpdateTimerId = 1;
  // 配合动态按需启停的 1ms 硬件调度周期，4ms 轮询可流畅覆盖 120Hz/144Hz/240Hz 高刷屏；
  // 隐藏时完全停止定时器与 1ms 时钟中断，实现待机 0% 唤醒。
  static constexpr UINT kUpdateIntervalMs = 4;

  struct ArmState {
    HWND hwnd = nullptr;
    ID2D1HwndRenderTarget* target = nullptr;
    RECT last_rect = {0, 0, 0, 0};
    bool shown = false;
  };

  static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM w_param,
                                  LPARAM l_param) noexcept;

  [[nodiscard]] bool InitializeDirect2D();
  void CleanupDirect2D() noexcept;
  void DestroyWindows() noexcept;
  void SetTrackingActive(bool active) noexcept;
  void InvalidateMonitorCache() noexcept;
  void UpdatePosition();
  void SetWindowsVisible(bool visible) noexcept;
  [[nodiscard]] HRESULT RenderArm(size_t index) const noexcept;

  HINSTANCE instance_ = nullptr;
  Config config_;
  bool visible_ = false;
  bool class_registered_ = false;
  bool timer_running_ = false;
  bool precision_timer_active_ = false;
  POINT last_cursor_pos_ = {LONG_MIN, LONG_MIN};

  HMONITOR cached_monitor_ = nullptr;
  RECT cached_bounds_ = {0, 0, 0, 0};
  float cached_dpi_scale_ = 1.0f;

  // 0: Left, 1: Right, 2: Top, 3: Bottom
  std::array<ArmState, 4> arms_{};
  ID2D1Factory* d2d_factory_ = nullptr;
};
