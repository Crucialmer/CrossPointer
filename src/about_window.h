#pragma once

#include <windows.h>

#include <memory>

#include "./liquid_glass.h"

class AboutWindow {
 public:
  AboutWindow() noexcept = default;
  ~AboutWindow();

  AboutWindow(const AboutWindow&) = delete;
  AboutWindow& operator=(const AboutWindow&) = delete;

  void Show(HINSTANCE instance, HWND parent);
  void NotifyThemeChanged() noexcept;
  void Close() noexcept;
  [[nodiscard]] bool IsOpen() const noexcept { return hwnd_ != nullptr; }

 private:
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp,
                                  LPARAM lp) noexcept;
  LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  void RebuildFonts(int dpi);
  void Paint(HDC screen_hdc);

  std::unique_ptr<liquid_glass::GdiplusSession> gdip_;
  HINSTANCE instance_ = nullptr;
  HWND parent_ = nullptr;
  HWND hwnd_ = nullptr;

  liquid_glass::Palette pal_;
  HFONT font_caption_ = nullptr;
  HFONT font_app_ = nullptr;
  HFONT font_badge_ = nullptr;
  HFONT font_sub_ = nullptr;
  HFONT font_feat_bold_ = nullptr;
  HFONT font_feat_ = nullptr;
  HFONT font_btn_ = nullptr;

  RECT rc_close_ = {};
  RECT rc_link_ = {};
  RECT rc_ok_ = {};
  bool hover_close_ = false;
  bool press_close_ = false;
  bool hover_link_ = false;
  bool hover_ok_ = false;
  bool press_ok_ = false;
  bool tracking_mouse_ = false;
};
