#pragma once

#include <windows.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "./liquid_glass.h"
#include "./updater.h"

class SoftwareUpdateWindow {
 public:
  SoftwareUpdateWindow() noexcept = default;
  ~SoftwareUpdateWindow();

  SoftwareUpdateWindow(const SoftwareUpdateWindow&) = delete;
  SoftwareUpdateWindow& operator=(const SoftwareUpdateWindow&) = delete;

  // 打开更新窗口并立即发起检查
  void ShowAndCheck(HINSTANCE instance, HWND controller);

  // 静默检查发现新版本时直接展示可用更新页
  void ShowWithResult(HINSTANCE instance, HWND controller,
                      const updater::UpdateResult& result);

  // 启动后后台静默检查（有新版本时通过 controller_msg 通知主线程弹窗）
  static void StartSilentCheck(HWND controller, UINT notify_msg);

  void NotifyThemeChanged() noexcept;
  void Close() noexcept;
  [[nodiscard]] bool IsOpen() const noexcept { return hwnd_ != nullptr; }

 private:
  static constexpr UINT_PTR kAnimTimerId = 5101;
  static constexpr UINT kMsgCheckDone = WM_APP + 210;
  static constexpr UINT kMsgDownloadProgress = WM_APP + 211;
  static constexpr UINT kMsgDownloadDone = WM_APP + 212;

  enum class State {
    kChecking,
    kUpToDate,
    kAvailable,
    kDownloading,
    kInstalling,
    kError,
  };

  struct ButtonSlot {
    RECT rc = {};
    std::wstring label;
    liquid_glass::PillStyle style = liquid_glass::PillStyle::kPrimary;
    int action_id = 0;  // 1:OK/Close, 2:UpdateNow, 3:OpenWeb, 4:Cancel, 5:Retry
  };

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp,
                                  LPARAM lp) noexcept;
  LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  bool EnsureWindowCreated(HINSTANCE instance, HWND controller);
  void RebuildFonts(int dpi);
  void BeginCheck();
  void BeginDownload();
  void UpdateAnimationTimer();
  void OpenReleaseWebpage() const;
  void BuildButtons(int w, int h, int dpi);
  void ExecuteAction(int action_id);
  void Paint(HDC screen_hdc);

  std::unique_ptr<liquid_glass::GdiplusSession> gdip_;
  HINSTANCE instance_ = nullptr;
  HWND controller_ = nullptr;
  HWND hwnd_ = nullptr;

  liquid_glass::Palette pal_;
  State state_ = State::kChecking;
  updater::UpdateResult result_;
  updater::DownloadProgress progress_;
  std::wstring last_check_time_;
  std::wstring error_text_;

  std::shared_ptr<std::atomic<bool>> cancel_flag_;
  float anim_phase_ = 0.0f;
  bool anim_timer_running_ = false;

  HFONT font_caption_ = nullptr;
  HFONT font_app_ = nullptr;
  HFONT font_badge_ = nullptr;
  HFONT font_status_ = nullptr;
  HFONT font_body_ = nullptr;
  HFONT font_small_ = nullptr;
  HFONT font_btn_ = nullptr;

  RECT rc_close_ = {};
  std::vector<ButtonSlot> buttons_;
  bool hover_close_ = false;
  bool press_close_ = false;
  int hover_btn_ = -1;
  int press_btn_ = -1;
  bool tracking_mouse_ = false;
};
