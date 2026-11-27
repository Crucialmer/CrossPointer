#pragma once

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

#include "./liquid_glass.h"

class TrayMenuWindow {
 public:
  TrayMenuWindow() noexcept = default;
  ~TrayMenuWindow();

  TrayMenuWindow(const TrayMenuWindow&) = delete;
  TrayMenuWindow& operator=(const TrayMenuWindow&) = delete;

  void Show(HINSTANCE instance, HWND owner, POINT anchor_pt,
            bool crosshair_visible, const std::wstring& hotkey_hint);
  void Close() noexcept;
  [[nodiscard]] bool IsOpen() const noexcept { return hwnd_ != nullptr; }

 private:
  enum class IconKind {
    kCrosshair,
    kSettings,
    kUpdate,
    kReload,
    kConfig,
    kAbout,
    kExit,
  };

  struct MenuItem {
    bool is_separator = false;
    UINT command_id = 0;
    std::wstring label;
    std::wstring shortcut;
    IconKind icon = IconKind::kCrosshair;
    bool destructive = false;
    bool emphasized = false;
    RECT rect = {};
  };

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp,
                                  LPARAM lp) noexcept;
  LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  void RebuildFonts(int dpi);
  void ComputeLayout(int dpi, int* out_w, int* out_h);
  void Paint(HDC screen_hdc);
  void TriggerItem(int index);
  [[nodiscard]] int HitTestItem(int x, int y) const noexcept;
  void MoveSelection(int delta);

  std::unique_ptr<liquid_glass::GdiplusSession> gdip_;
  HINSTANCE instance_ = nullptr;
  HWND owner_ = nullptr;
  HWND hwnd_ = nullptr;
  bool crosshair_visible_ = false;
  std::wstring hotkey_hint_;

  liquid_glass::Palette pal_;
  std::vector<MenuItem> items_;
  RECT header_rect_ = {};
  int hover_index_ = -1;
  bool tracking_mouse_ = false;

  HFONT font_brand_ = nullptr;
  HFONT font_status_ = nullptr;
  HFONT font_item_ = nullptr;
  HFONT font_item_bold_ = nullptr;
  HFONT font_hint_ = nullptr;
};
