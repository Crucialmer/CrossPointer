#pragma once

#include <windows.h>
#include <objidl.h>

#include <algorithm>
#include <string>

namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

namespace liquid_glass {

// 引用计数 GDI+ 会话管理，确保多窗口并发开关安全无泄露
class GdiplusSession {
 public:
  GdiplusSession() noexcept;
  ~GdiplusSession() noexcept;
  GdiplusSession(const GdiplusSession&) = delete;
  GdiplusSession& operator=(const GdiplusSession&) = delete;

 private:
  bool active_ = false;
};

struct Palette {
  bool dark = false;
  COLORREF bg = RGB(241, 243, 247);
  COLORREF bg_bottom = RGB(234, 237, 243);
  COLORREF card = RGB(252, 253, 255);
  COLORREF card_bottom = RGB(247, 249, 253);
  COLORREF text = RGB(26, 28, 32);
  COLORREF secondary = RGB(114, 118, 128);
  COLORREF border = RGB(214, 220, 230);
  COLORREF divider = RGB(228, 232, 240);
  COLORREF accent = RGB(0, 122, 255);
  COLORREF green = RGB(40, 185, 76);
  COLORREF amber = RGB(245, 158, 11);
  COLORREF red = RGB(239, 68, 68);
};

[[nodiscard]] Palette GetPalette(bool dark) noexcept;
[[nodiscard]] Gdiplus::Color ToGdip(COLORREF c, BYTE alpha = 255) noexcept;

// DPI & 显示器辅助
[[nodiscard]] int GetWindowDpi(HWND hwnd) noexcept;
[[nodiscard]] int GetMonitorDpiForPoint(POINT pt) noexcept;
[[nodiscard]] RECT GetMonitorWorkAreaForPoint(POINT pt) noexcept;
void AdjustWindowRectForDpiCompat(RECT* rc, DWORD style, DWORD ex_style,
                                  int dpi) noexcept;
[[nodiscard]] int Scale(int value, int dpi) noexcept;
[[nodiscard]] float ScaleF(float value, int dpi) noexcept;
[[nodiscard]] HFONT CreateModernFont(int pt_size_x10, int weight, int dpi);

// Windows 11/10 DWM 液态玻璃圆角与边框适配
void ApplyWindowChrome(HWND hwnd, const Palette& pal,
                       bool borderless = true) noexcept;

// GDI+ 路径与 macOS 26 (Tahoe) Liquid Glass 绘图原语
void BuildRoundedPath(Gdiplus::GraphicsPath& path, float x, float y, float w,
                      float h, float r);

void DrawWindowBackground(Gdiplus::Graphics& g, float w, float h,
                          const Palette& pal, int dpi);

void DrawLiquidGlassCard(Gdiplus::Graphics& g, float x, float y, float w,
                         float h, float r, const Palette& pal, int dpi);

void DrawAppSquircleIcon(Gdiplus::Graphics& g, float x, float y, float size,
                         const Palette& pal, int dpi);

// macOS 左上角红灯关闭按钮 (#FF5F57)
void DrawTrafficLightClose(Gdiplus::Graphics& g, float cx, float cy, float r,
                           bool hovered, bool pressed, int dpi);

enum class PillStyle { kPrimary, kSecondary, kDanger };

void DrawPillButton(Gdiplus::Graphics& g, HDC hdc, const RECT& rc,
                    const wchar_t* text, HFONT font, PillStyle style,
                    bool hovered, bool pressed, bool focused,
                    const Palette& pal, int dpi);

void DrawBadgePill(Gdiplus::Graphics& g, HDC hdc, const RECT& rc,
                   const wchar_t* text, HFONT font, COLORREF tint,
                   const Palette& pal, int dpi);

enum class OrbKind { kInfo, kSuccess, kUpdate, kWarning, kError };

void DrawStatusOrb(Gdiplus::Graphics& g, float cx, float cy, float r,
                   OrbKind kind, const Palette& pal, int dpi);

void DrawProgressBar(Gdiplus::Graphics& g, float x, float y, float w, float h,
                     float ratio, bool indeterminate, float anim_phase,
                     const Palette& pal, int dpi);

// macOS 26 Liquid Glass 模态提示弹窗（全面替代原生 Win32 MessageBoxW）
void ShowAlert(HWND parent, const std::wstring& title,
               const std::wstring& message, OrbKind kind = OrbKind::kInfo,
               const std::wstring& button_text = L"好");

}  // namespace liquid_glass
