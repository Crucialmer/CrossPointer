#pragma once

#include <windows.h>
#include <commctrl.h>

#include <functional>
#include <memory>
#include <string>

#include "./config.h"
#include "./liquid_glass.h"

class SettingsWindow {
 public:
  using ApplyCallback = std::function<bool(const Config& new_config)>;

  SettingsWindow() noexcept = default;
  ~SettingsWindow();

  SettingsWindow(const SettingsWindow&) = delete;
  SettingsWindow& operator=(const SettingsWindow&) = delete;

  // 打开或激活偏好设置窗口
  void Show(HINSTANCE instance, HWND parent, const Config& current_config,
            const std::wstring& config_path, bool crosshair_visible,
            ApplyCallback on_apply);

  void NotifyThemeChanged() noexcept;
  void NotifyVisibilityChanged(bool crosshair_visible) noexcept;
  void Close() noexcept;

  [[nodiscard]] bool IsOpen() const noexcept { return hwnd_ != nullptr; }
  [[nodiscard]] HWND GetHwnd() const noexcept { return hwnd_; }

 private:
  static constexpr UINT_PTR kToastTimerId = 4001;

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp,
                                  LPARAM lp) noexcept;
  static LRESULT CALLBACK ControlHoverSubclassProc(HWND ctrl, UINT msg,
                                                   WPARAM wp, LPARAM lp,
                                                   UINT_PTR subclass_id,
                                                   DWORD_PTR ref_data) noexcept;
  static LRESULT CALLBACK SliderSubclassProc(HWND ctrl, UINT msg, WPARAM wp,
                                             LPARAM lp, UINT_PTR subclass_id,
                                             DWORD_PTR ref_data) noexcept;

  LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  void PaintSliderControl(HWND slider, HDC screen_hdc);
  void DrawOwnerControl(const DRAWITEMSTRUCT* dis);
  void PaintWindow(HDC screen_hdc);

  void CreateControls();
  void LayoutControls(int dpi);
  void UpdateThemeStyles();
  void LoadValuesToUI();
  bool SaveValuesFromUI();
  void SwitchTab(int new_tab);
  void UpdateTabVisibility();
  void ShowToast(const std::wstring& text, bool success = true);
  void SyncColorTabControls();
  [[nodiscard]] std::wstring FormatShortcutPreview(unsigned int mod,
                                                   unsigned int vk) const;
  [[nodiscard]] unsigned int GetSelectedVk(HWND combo,
                                           unsigned int fallback) const;

  HINSTANCE instance_ = nullptr;
  HWND parent_ = nullptr;
  HWND hwnd_ = nullptr;
  std::wstring config_path_;
  Config config_;
  ApplyCallback on_apply_;
  bool crosshair_visible_ = false;

  std::unique_ptr<liquid_glass::GdiplusSession> gdip_;

  HFONT font_title_ = nullptr;
  HFONT font_card_title_ = nullptr;
  HFONT font_bold_ = nullptr;
  HFONT font_regular_ = nullptr;
  HFONT font_small_ = nullptr;

  int current_tab_ = 0;  // 0: 外观尺寸, 1: 色彩模式, 2: 全局快捷键, 3: 系统与更新
  int hover_tab_ = -1;
  bool tracking_mouse_ = false;
  HWND hovered_ctrl_ = nullptr;

  // Inline toast state
  std::wstring toast_text_;
  bool toast_success_ = true;
  bool toast_visible_ = false;

  // Tab 0: 外观尺寸
  HWND slider_gap_ = nullptr;
  HWND lbl_gap_val_ = nullptr;
  HWND slider_h_width_ = nullptr;
  HWND lbl_h_width_val_ = nullptr;
  HWND slider_v_width_ = nullptr;
  HWND lbl_v_width_val_ = nullptr;
  HWND preview_crosshair_ = nullptr;

  // Tab 1: 色彩与模式
  HWND sw_auto_theme_ = nullptr;
  bool draft_auto_theme_ = true;

  HWND preview_color_a_ = nullptr;
  HWND btn_color_a_ = nullptr;
  HWND slider_alpha_a_ = nullptr;
  HWND lbl_alpha_a_val_ = nullptr;

  HWND preview_color_b_ = nullptr;
  HWND btn_color_b_ = nullptr;
  HWND slider_alpha_b_ = nullptr;
  HWND lbl_alpha_b_val_ = nullptr;

  LineColor temp_light_color_;
  LineColor temp_dark_color_;
  LineColor temp_horiz_color_;
  LineColor temp_vert_color_;

  // Tab 2: 快捷键 (macOS 键帽胶囊 + 按键下拉)
  HWND key_toggle_ctrl_ = nullptr;
  HWND key_toggle_alt_ = nullptr;
  HWND key_toggle_win_ = nullptr;
  HWND key_toggle_shift_ = nullptr;
  HWND combo_toggle_vk_ = nullptr;
  unsigned int draft_toggle_mod_ = MOD_SHIFT | MOD_ALT;

  HWND key_exit_ctrl_ = nullptr;
  HWND key_exit_alt_ = nullptr;
  HWND key_exit_win_ = nullptr;
  HWND key_exit_shift_ = nullptr;
  HWND combo_exit_vk_ = nullptr;
  unsigned int draft_exit_mod_ = MOD_SHIFT | MOD_ALT;

  HWND btn_sync_hotkey_ = nullptr;

  // Tab 3: 系统与更新
  HWND sw_show_on_startup_ = nullptr;
  HWND sw_auto_dpi_ = nullptr;
  HWND sw_autostart_ = nullptr;
  HWND sw_auto_check_update_ = nullptr;
  HWND btn_check_update_ = nullptr;

  bool draft_show_on_startup_ = false;
  bool draft_auto_dpi_ = true;
  bool draft_autostart_ = false;
  bool draft_auto_check_update_ = true;

  // 底部统一动作栏
  HWND btn_save_ = nullptr;
  HWND btn_defaults_ = nullptr;
  HWND btn_close_ = nullptr;

  HBRUSH br_bg_ = nullptr;
  HBRUSH br_card_ = nullptr;
  HBRUSH br_combo_ = nullptr;

  COLORREF cr_bg_ = RGB(242, 243, 247);
  COLORREF cr_card_ = RGB(255, 255, 255);
  COLORREF cr_text_ = RGB(29, 29, 31);
  COLORREF cr_secondary_ = RGB(126, 128, 134);
  COLORREF cr_card_border_ = RGB(218, 222, 230);
  COLORREF cr_divider_ = RGB(232, 234, 240);
  COLORREF cr_accent_ = RGB(0, 122, 255);
  COLORREF cr_green_ = RGB(52, 199, 89);
  bool is_dark_mode_ = false;
};
