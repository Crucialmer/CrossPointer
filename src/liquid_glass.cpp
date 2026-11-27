#include "./liquid_glass.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <atomic>
#include <cmath>
#include <mutex>

#include "./config.h"

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif

namespace liquid_glass {
namespace {

std::mutex g_gdip_mutex;
int g_gdip_ref_count = 0;
ULONG_PTR g_gdip_token = 0;

constexpr wchar_t kAlertClassName[] = L"CrossPointerGlassAlertClass";

struct AlertState {
  std::wstring title;
  std::wstring message;
  std::wstring button_text;
  OrbKind kind = OrbKind::kInfo;
  Palette pal;
  HFONT font_title = nullptr;
  HFONT font_body = nullptr;
  HFONT font_btn = nullptr;
  HFONT font_small = nullptr;
  RECT rc_close = {};
  RECT rc_btn = {};
  bool hover_close = false;
  bool press_close = false;
  bool hover_btn = false;
  bool press_btn = false;
  bool tracking = false;
  bool done = false;
};

void RebuildAlertFonts(AlertState* st, int dpi) {
  if (st->font_title) DeleteObject(st->font_title);
  if (st->font_body) DeleteObject(st->font_body);
  if (st->font_btn) DeleteObject(st->font_btn);
  if (st->font_small) DeleteObject(st->font_small);
  st->font_title = CreateModernFont(115, FW_SEMIBOLD, dpi);
  st->font_body = CreateModernFont(92, FW_NORMAL, dpi);
  st->font_btn = CreateModernFont(95, FW_SEMIBOLD, dpi);
  st->font_small = CreateModernFont(85, FW_NORMAL, dpi);
}

bool PtInRectF(const RECT& r, int x, int y) noexcept {
  return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

LRESULT CALLBACK AlertWndProc(HWND hwnd, UINT msg, WPARAM wp,
                              LPARAM lp) noexcept {
  auto* st =
      reinterpret_cast<AlertState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_NCCREATE) {
    const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lp);
    st = static_cast<AlertState*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
    return DefWindowProcW(hwnd, msg, wp, lp);
  }
  if (st == nullptr) return DefWindowProcW(hwnd, msg, wp, lp);

  switch (msg) {
    case WM_ERASEBKGND:
      return 1;

    case WM_SETTINGCHANGE: {
      st->pal = GetPalette(!IsSystemInLightTheme());
      ApplyWindowChrome(hwnd, st->pal, true);
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }

    case WM_DPICHANGED: {
      const int dpi = LOWORD(wp);
      const auto* prc = reinterpret_cast<const RECT*>(lp);
      SetWindowPos(hwnd, nullptr, prc->left, prc->top, prc->right - prc->left,
                   prc->bottom - prc->top, SWP_NOZORDER | SWP_NOACTIVATE);
      RebuildAlertFonts(st, dpi);
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }

    case WM_KEYDOWN: {
      if (wp == VK_ESCAPE || wp == VK_RETURN || wp == VK_SPACE) {
        st->done = true;
        DestroyWindow(hwnd);
        return 0;
      }
      break;
    }

    case WM_MOUSEMOVE: {
      if (!st->tracking) {
        TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
        st->tracking = true;
      }
      const int mx = GET_X_LPARAM(lp);
      const int my = GET_Y_LPARAM(lp);
      const bool hc = PtInRectF(st->rc_close, mx, my);
      const bool hb = PtInRectF(st->rc_btn, mx, my);
      if (hc != st->hover_close || hb != st->hover_btn) {
        st->hover_close = hc;
        st->hover_btn = hb;
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    }

    case WM_MOUSELEAVE: {
      st->tracking = false;
      if (st->hover_close || st->hover_btn) {
        st->hover_close = false;
        st->hover_btn = false;
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    }

    case WM_SETCURSOR: {
      if (st->hover_close || st->hover_btn) {
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
      }
      break;
    }

    case WM_LBUTTONDOWN: {
      const int mx = GET_X_LPARAM(lp);
      const int my = GET_Y_LPARAM(lp);
      if (PtInRectF(st->rc_close, mx, my)) {
        st->press_close = true;
        SetCapture(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      if (PtInRectF(st->rc_btn, mx, my)) {
        st->press_btn = true;
        SetCapture(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      // 允许拖拽窗口任意空白区移动
      ReleaseCapture();
      SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
      return 0;
    }

    case WM_LBUTTONUP: {
      const int mx = GET_X_LPARAM(lp);
      const int my = GET_Y_LPARAM(lp);
      if (GetCapture() == hwnd) ReleaseCapture();
      const bool was_close = st->press_close;
      const bool was_btn = st->press_btn;
      st->press_close = false;
      st->press_btn = false;
      if ((was_close && PtInRectF(st->rc_close, mx, my)) ||
          (was_btn && PtInRectF(st->rc_btn, mx, my))) {
        st->done = true;
        DestroyWindow(hwnd);
        return 0;
      }
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }

    case WM_PAINT: {
      PAINTSTRUCT ps = {};
      HDC screen_hdc = BeginPaint(hwnd, &ps);
      RECT rc = {};
      GetClientRect(hwnd, &rc);
      const int w = rc.right - rc.left;
      const int h = rc.bottom - rc.top;
      if (w > 0 && h > 0) {
        const int dpi = GetWindowDpi(hwnd);
        HDC hdc = CreateCompatibleDC(screen_hdc);
        HBITMAP bmp = CreateCompatibleBitmap(screen_hdc, w, h);
        HBITMAP old_bmp = static_cast<HBITMAP>(SelectObject(hdc, bmp));

        // 计算交互区
        const int close_cx = Scale(20, dpi);
        const int close_cy = Scale(20, dpi);
        const int close_hit = Scale(11, dpi);
        st->rc_close = {close_cx - close_hit, close_cy - close_hit,
                        close_cx + close_hit, close_cy + close_hit};

        const int btn_w = Scale(124, dpi);
        const int btn_h = Scale(34, dpi);
        st->rc_btn = {(w - btn_w) / 2, h - Scale(52, dpi), (w + btn_w) / 2,
                      h - Scale(52, dpi) + btn_h};

        {
          Gdiplus::Graphics g(hdc);
          g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
          g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

          DrawWindowBackground(g, static_cast<float>(w), static_cast<float>(h),
                               st->pal, dpi);
          DrawTrafficLightClose(g, static_cast<float>(close_cx),
                                static_cast<float>(close_cy), ScaleF(6.5f, dpi),
                                st->hover_close, st->press_close, dpi);

          // 主体液态玻璃内容卡
          const float card_x = ScaleF(20.0f, dpi);
          const float card_y = ScaleF(42.0f, dpi);
          const float card_w = static_cast<float>(w) - card_x * 2.0f;
          const float card_h = static_cast<float>(h) - ScaleF(110.0f, dpi);
          DrawLiquidGlassCard(g, card_x, card_y, card_w, card_h,
                              ScaleF(14.0f, dpi), st->pal, dpi);

          // 顶部光学状态圆球
          DrawStatusOrb(g, static_cast<float>(w) * 0.5f,
                        card_y + ScaleF(38.0f, dpi), ScaleF(20.0f, dpi),
                        st->kind, st->pal, dpi);

          // 底部主胶囊按钮
          DrawPillButton(g, hdc, st->rc_btn, st->button_text.c_str(),
                         st->font_btn, PillStyle::kPrimary, st->hover_btn,
                         st->press_btn, true, st->pal, dpi);
        }

        SetBkMode(hdc, TRANSPARENT);
        SelectFont(hdc, st->font_small);
        SetTextColor(hdc, st->pal.secondary);
        RECT cap_rc = {Scale(40, dpi), Scale(10, dpi), w - Scale(40, dpi),
                       Scale(30, dpi)};
        DrawTextW(hdc, L"CrossPointer", -1, &cap_rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        SelectFont(hdc, st->font_title);
        SetTextColor(hdc, st->pal.text);
        RECT t_rc = {Scale(36, dpi), Scale(108, dpi), w - Scale(36, dpi),
                     Scale(136, dpi)};
        DrawTextW(hdc, st->title.c_str(), -1, &t_rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        SelectFont(hdc, st->font_body);
        SetTextColor(hdc, st->pal.secondary);
        RECT m_rc = {Scale(36, dpi), Scale(142, dpi), w - Scale(36, dpi),
                     h - Scale(76, dpi)};
        DrawTextW(hdc, st->message.c_str(), -1, &m_rc,
                  DT_CENTER | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);

        BitBlt(screen_hdc, 0, 0, w, h, hdc, 0, 0, SRCCOPY);
        SelectObject(hdc, old_bmp);
        DeleteObject(bmp);
        DeleteDC(hdc);
      }
      EndPaint(hwnd, &ps);
      return 0;
    }

    case WM_CLOSE: {
      st->done = true;
      DestroyWindow(hwnd);
      return 0;
    }
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

GdiplusSession::GdiplusSession() noexcept {
  std::lock_guard<std::mutex> lock(g_gdip_mutex);
  if (g_gdip_ref_count == 0) {
    Gdiplus::GdiplusStartupInput input;
    if (Gdiplus::GdiplusStartup(&g_gdip_token, &input, nullptr) ==
        Gdiplus::Ok) {
      g_gdip_ref_count = 1;
      active_ = true;
    }
  } else {
    ++g_gdip_ref_count;
    active_ = true;
  }
}

GdiplusSession::~GdiplusSession() noexcept {
  if (!active_) return;
  std::lock_guard<std::mutex> lock(g_gdip_mutex);
  if (g_gdip_ref_count > 0) {
    --g_gdip_ref_count;
    if (g_gdip_ref_count == 0 && g_gdip_token != 0) {
      Gdiplus::GdiplusShutdown(g_gdip_token);
      g_gdip_token = 0;
    }
  }
}

Palette GetPalette(bool dark) noexcept {
  Palette p;
  p.dark = dark;
  if (dark) {
    p.bg = RGB(28, 29, 34);
    p.bg_bottom = RGB(22, 23, 27);
    p.card = RGB(45, 47, 54);
    p.card_bottom = RGB(37, 39, 45);
    p.text = RGB(245, 246, 250);
    p.secondary = RGB(154, 158, 168);
    p.border = RGB(68, 72, 82);
    p.divider = RGB(58, 61, 70);
    p.accent = RGB(10, 132, 255);
    p.green = RGB(48, 209, 88);
    p.amber = RGB(255, 179, 64);
    p.red = RGB(255, 69, 58);
  } else {
    p.bg = RGB(244, 246, 250);
    p.bg_bottom = RGB(236, 239, 245);
    p.card = RGB(255, 255, 255);
    p.card_bottom = RGB(247, 249, 253);
    p.text = RGB(26, 28, 32);
    p.secondary = RGB(112, 116, 126);
    p.border = RGB(214, 220, 230);
    p.divider = RGB(229, 233, 241);
    p.accent = RGB(0, 122, 255);
    p.green = RGB(40, 185, 76);
    p.amber = RGB(234, 140, 0);
    p.red = RGB(229, 57, 53);
  }
  return p;
}

Gdiplus::Color ToGdip(COLORREF c, BYTE alpha) noexcept {
  return Gdiplus::Color(alpha, GetRValue(c), GetGValue(c), GetBValue(c));
}

int GetWindowDpi(HWND hwnd) noexcept {
  using GetDpiFn = UINT(WINAPI*)(HWND);
  static const auto fn = []() -> GetDpiFn {
    if (const HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
      return reinterpret_cast<GetDpiFn>(
          GetProcAddress(user32, "GetDpiForWindow"));
    }
    return nullptr;
  }();
  if (fn != nullptr && hwnd != nullptr) {
    const UINT dpi = fn(hwnd);
    if (dpi > 0) return static_cast<int>(dpi);
  }
  const HDC hdc = GetDC(nullptr);
  const int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
  ReleaseDC(nullptr, hdc);
  return dpi > 0 ? dpi : 96;
}

int GetMonitorDpiForPoint(POINT pt) noexcept {
  using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
  static const auto fn = []() -> GetDpiForMonitorFn {
    const HMODULE shcore = LoadLibraryExW(L"shcore.dll", nullptr,
                                          LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (shcore != nullptr) {
      return reinterpret_cast<GetDpiForMonitorFn>(
          GetProcAddress(shcore, "GetDpiForMonitor"));
    }
    return nullptr;
  }();
  if (fn != nullptr) {
    const HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    UINT dx = 96;
    UINT dy = 96;
    if (mon != nullptr && SUCCEEDED(fn(mon, 0, &dx, &dy)) && dx > 0) {
      return static_cast<int>(dx);
    }
  }
  const HDC hdc = GetDC(nullptr);
  const int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
  ReleaseDC(nullptr, hdc);
  return dpi > 0 ? dpi : 96;
}

RECT GetMonitorWorkAreaForPoint(POINT pt) noexcept {
  const HMONITOR monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi = {sizeof(mi)};
  if (monitor != nullptr && GetMonitorInfoW(monitor, &mi) != FALSE) {
    return mi.rcWork;
  }
  return {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
}

void AdjustWindowRectForDpiCompat(RECT* rc, DWORD style, DWORD ex_style,
                                  int dpi) noexcept {
  using AdjustFn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
  static const auto fn = []() -> AdjustFn {
    if (const HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
      return reinterpret_cast<AdjustFn>(
          GetProcAddress(user32, "AdjustWindowRectExForDpi"));
    }
    return nullptr;
  }();
  if (fn != nullptr &&
      fn(rc, style, FALSE, ex_style, static_cast<UINT>(dpi)) != FALSE) {
    return;
  }
  AdjustWindowRectEx(rc, style, FALSE, ex_style);
}

int Scale(int value, int dpi) noexcept { return MulDiv(value, dpi, 96); }

float ScaleF(float value, int dpi) noexcept {
  return value * (static_cast<float>(dpi) / 96.0f);
}

HFONT CreateModernFont(int pt_size_x10, int weight, int dpi) {
  const int height = -MulDiv(pt_size_x10, dpi, 720);
  return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                     CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                     L"Microsoft YaHei UI");
}

void ApplyWindowChrome(HWND hwnd, const Palette& pal,
                       bool borderless) noexcept {
  if (hwnd == nullptr) return;
  const DWMNCRENDERINGPOLICY ncp = DWMNCRP_ENABLED;
  DwmSetWindowAttribute(hwnd, DWMWA_NCRENDERING_POLICY, &ncp, sizeof(ncp));

  const DWORD round_pref = DWMWCP_ROUND;
  DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &round_pref,
                        sizeof(round_pref));

  const BOOL dark_attr = pal.dark ? TRUE : FALSE;
  DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark_attr,
                        sizeof(dark_attr));

  const COLORREF border_col = pal.border;
  DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border_col,
                        sizeof(border_col));

  if (!borderless) {
    const COLORREF caption_col = pal.bg;
    DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &caption_col,
                          sizeof(caption_col));
  }
}

void BuildRoundedPath(Gdiplus::GraphicsPath& path, float x, float y, float w,
                      float h, float r) {
  const float max_r = std::min(w, h) * 0.5f;
  const float rad = std::clamp(r, 0.5f, max_r);
  const float d = rad * 2.0f;
  path.Reset();
  path.AddArc(x, y, d, d, 180.0f, 90.0f);
  path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
  path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
  path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
  path.CloseFigure();
}

void DrawWindowBackground(Gdiplus::Graphics& g, float w, float h,
                          const Palette& pal, int dpi) {
  Gdiplus::LinearGradientBrush bg_br(
      Gdiplus::PointF(0.0f, 0.0f), Gdiplus::PointF(0.0f, h), ToGdip(pal.bg),
      ToGdip(pal.bg_bottom));
  g.FillRectangle(&bg_br, 0.0f, 0.0f, w, h);

  // 顶部光学折射高光边缘 (Liquid Glass Specular Top Edge)
  Gdiplus::Pen top_sheen(
      pal.dark ? Gdiplus::Color(46, 255, 255, 255)
               : Gdiplus::Color(220, 255, 255, 255),
      ScaleF(1.2f, dpi));
  g.DrawLine(&top_sheen, ScaleF(10.0f, dpi), ScaleF(0.8f, dpi),
             w - ScaleF(10.0f, dpi), ScaleF(0.8f, dpi));
}

void DrawLiquidGlassCard(Gdiplus::Graphics& g, float x, float y, float w,
                         float h, float r, const Palette& pal, int dpi) {
  for (int i = 2; i >= 1; --i) {
    Gdiplus::GraphicsPath shadow_path;
    const float spread = ScaleF(static_cast<float>(i), dpi);
    BuildRoundedPath(shadow_path, x - spread * 0.35f, y + spread * 0.85f,
                     w + spread * 0.7f, h + spread * 0.55f, r + spread * 0.4f);
    const BYTE a =
        pal.dark ? static_cast<BYTE>(28 / i) : static_cast<BYTE>(13 / i);
    Gdiplus::SolidBrush shadow_br(Gdiplus::Color(a, 0, 0, 0));
    g.FillPath(&shadow_br, &shadow_path);
  }

  Gdiplus::GraphicsPath card_path;
  BuildRoundedPath(card_path, x, y, w, h, r);
  Gdiplus::LinearGradientBrush fill_br(
      Gdiplus::PointF(x, y), Gdiplus::PointF(x, y + h), ToGdip(pal.card),
      ToGdip(pal.card_bottom));
  g.FillPath(&fill_br, &card_path);

  Gdiplus::GraphicsState st = g.Save();
  g.SetClip(&card_path);
  Gdiplus::Pen spec_pen(
      pal.dark ? Gdiplus::Color(48, 255, 255, 255)
               : Gdiplus::Color(225, 255, 255, 255),
      ScaleF(1.2f, dpi));
  g.DrawLine(&spec_pen, x + r * 0.55f, y + ScaleF(1.0f, dpi),
             x + w - r * 0.55f, y + ScaleF(1.0f, dpi));
  g.Restore(st);

  Gdiplus::Pen border_pen(
      pal.dark ? Gdiplus::Color(62, 255, 255, 255) : ToGdip(pal.border),
      ScaleF(1.0f, dpi));
  g.DrawPath(&border_pen, &card_path);
}

void DrawAppSquircleIcon(Gdiplus::Graphics& g, float x, float y, float size,
                         const Palette& pal, int dpi) {
  const float r = size * 0.27f;
  Gdiplus::GraphicsPath icon_path;
  BuildRoundedPath(icon_path, x, y, size, size, r);

  // 柔和微投影
  Gdiplus::GraphicsPath sh_path;
  BuildRoundedPath(sh_path, x, y + ScaleF(1.8f, dpi), size, size, r);
  Gdiplus::SolidBrush sh_br(Gdiplus::Color(pal.dark ? 75 : 35, 0, 0, 0));
  g.FillPath(&sh_br, &sh_path);

  Gdiplus::LinearGradientBrush bg_br(
      Gdiplus::PointF(x, y), Gdiplus::PointF(x, y + size),
      Gdiplus::Color(255, 42, 54, 72), Gdiplus::Color(255, 19, 25, 36));
  g.FillPath(&bg_br, &icon_path);

  Gdiplus::Pen rim(Gdiplus::Color(95, 165, 218, 255),
                   std::max(1.0f, ScaleF(1.0f, dpi)));
  g.DrawPath(&rim, &icon_path);

  const float cx = x + size * 0.5f;
  const float cy = y + size * 0.5f;
  const float arm_in = size * 0.13f;
  const float arm_out = size * 0.34f;

  Gdiplus::Pen glow_pen(Gdiplus::Color(70, 0, 210, 255), size * 0.105f);
  glow_pen.SetStartCap(Gdiplus::LineCapRound);
  glow_pen.SetEndCap(Gdiplus::LineCapRound);

  Gdiplus::Pen laser_pen(Gdiplus::Color(255, 0, 224, 255),
                         std::max(1.4f, size * 0.052f));
  laser_pen.SetStartCap(Gdiplus::LineCapRound);
  laser_pen.SetEndCap(Gdiplus::LineCapRound);

  for (Gdiplus::Pen* p : {&glow_pen, &laser_pen}) {
    g.DrawLine(p, cx - arm_out, cy, cx - arm_in, cy);
    g.DrawLine(p, cx + arm_in, cy, cx + arm_out, cy);
    g.DrawLine(p, cx, cy - arm_out, cx, cy - arm_in);
    g.DrawLine(p, cx, cy + arm_in, cx, cy + arm_out);
  }

  const float ring_r = size * 0.085f;
  Gdiplus::Pen ring_pen(Gdiplus::Color(238, 224, 249, 255),
                        std::max(1.0f, size * 0.03f));
  g.DrawEllipse(&ring_pen, cx - ring_r, cy - ring_r, ring_r * 2.0f,
                ring_r * 2.0f);
}

void DrawTrafficLightClose(Gdiplus::Graphics& g, float cx, float cy, float r,
                           bool hovered, bool pressed, int dpi) {
  const Gdiplus::Color fill =
      pressed ? Gdiplus::Color(255, 206, 68, 60)
              : (hovered ? Gdiplus::Color(255, 255, 86, 78)
                         : Gdiplus::Color(255, 255, 95, 87));
  Gdiplus::SolidBrush br(fill);
  g.FillEllipse(&br, cx - r, cy - r, r * 2.0f, r * 2.0f);

  Gdiplus::Pen rim(Gdiplus::Color(90, 180, 42, 34),
                   std::max(0.8f, ScaleF(0.9f, dpi)));
  g.DrawEllipse(&rim, cx - r, cy - r, r * 2.0f, r * 2.0f);

  if (hovered || pressed) {
    const float cr = r * 0.42f;
    Gdiplus::Pen x_pen(Gdiplus::Color(210, 92, 16, 12),
                       std::max(1.2f, ScaleF(1.35f, dpi)));
    x_pen.SetStartCap(Gdiplus::LineCapRound);
    x_pen.SetEndCap(Gdiplus::LineCapRound);
    g.DrawLine(&x_pen, cx - cr, cy - cr, cx + cr, cy + cr);
    g.DrawLine(&x_pen, cx + cr, cy - cr, cx - cr, cy + cr);
  }
}

void DrawPillButton(Gdiplus::Graphics& g, HDC hdc, const RECT& rc,
                    const wchar_t* text, HFONT font, PillStyle style,
                    bool hovered, bool pressed, bool focused,
                    const Palette& pal, int dpi) {
  const float x = static_cast<float>(rc.left) + ScaleF(0.8f, dpi);
  const float y = static_cast<float>(rc.top) + ScaleF(0.8f, dpi);
  const float w = static_cast<float>(rc.right - rc.left) - ScaleF(1.6f, dpi);
  const float h = static_cast<float>(rc.bottom - rc.top) - ScaleF(1.6f, dpi);
  if (w <= 2.0f || h <= 2.0f) return;
  const float rad = h * 0.5f;

  Gdiplus::GraphicsPath pill;
  BuildRoundedPath(pill, x, y, w, h, rad);

  COLORREF txt_col = pal.text;
  if (style == PillStyle::kPrimary) {
    const Gdiplus::Color top =
        pressed ? Gdiplus::Color(255, 0, 94, 208)
                : (hovered ? Gdiplus::Color(255, 34, 146, 255)
                           : Gdiplus::Color(255, 16, 134, 255));
    const Gdiplus::Color bot =
        pressed ? Gdiplus::Color(255, 0, 82, 184)
                : Gdiplus::Color(255, 0, 108, 232);
    Gdiplus::LinearGradientBrush br(Gdiplus::PointF(x, y),
                                    Gdiplus::PointF(x, y + h), top, bot);
    g.FillPath(&br, &pill);

    Gdiplus::GraphicsState st = g.Save();
    g.SetClip(&pill);
    Gdiplus::Pen sheen(Gdiplus::Color(92, 255, 255, 255), ScaleF(1.2f, dpi));
    g.DrawLine(&sheen, x + rad * 0.5f, y + ScaleF(1.0f, dpi),
               x + w - rad * 0.5f, y + ScaleF(1.0f, dpi));
    g.Restore(st);

    Gdiplus::Pen rim(Gdiplus::Color(70, 255, 255, 255), ScaleF(1.0f, dpi));
    g.DrawPath(&rim, &pill);
    txt_col = RGB(255, 255, 255);
  } else if (style == PillStyle::kDanger) {
    const Gdiplus::Color top =
        pressed ? Gdiplus::Color(255, 198, 42, 38)
                : (hovered ? Gdiplus::Color(255, 255, 86, 78)
                           : ToGdip(pal.red));
    const Gdiplus::Color bot = Gdiplus::Color(255, 212, 48, 42);
    Gdiplus::LinearGradientBrush br(Gdiplus::PointF(x, y),
                                    Gdiplus::PointF(x, y + h), top, bot);
    g.FillPath(&br, &pill);
    txt_col = RGB(255, 255, 255);
  } else {
    const Gdiplus::Color top =
        pal.dark ? (pressed ? Gdiplus::Color(255, 50, 53, 60)
                            : (hovered ? Gdiplus::Color(255, 76, 80, 92)
                                       : Gdiplus::Color(255, 62, 65, 74)))
                 : (pressed ? Gdiplus::Color(255, 224, 229, 238)
                            : (hovered ? Gdiplus::Color(255, 255, 255, 255)
                                       : Gdiplus::Color(255, 249, 251, 254)));
    const Gdiplus::Color bot =
        pal.dark ? (pressed ? Gdiplus::Color(255, 44, 46, 52)
                            : (hovered ? Gdiplus::Color(255, 64, 68, 78)
                                       : Gdiplus::Color(255, 52, 55, 63)))
                 : (pressed ? Gdiplus::Color(255, 216, 222, 232)
                            : (hovered ? Gdiplus::Color(255, 242, 246, 252)
                                       : Gdiplus::Color(255, 237, 241, 248)));
    Gdiplus::LinearGradientBrush br(Gdiplus::PointF(x, y),
                                    Gdiplus::PointF(x, y + h), top, bot);
    g.FillPath(&br, &pill);

    Gdiplus::Pen rim(
        focused ? ToGdip(pal.accent)
                : (pal.dark ? Gdiplus::Color(70, 255, 255, 255)
                            : Gdiplus::Color(255, 206, 213, 224)),
        ScaleF(1.0f, dpi));
    g.DrawPath(&rim, &pill);
  }

  if (text != nullptr && *text != L'\0') {
    RECT trc = rc;
    SetBkMode(hdc, TRANSPARENT);
    HFONT old_f = SelectFont(hdc, font);
    SetTextColor(hdc, txt_col);
    DrawTextW(hdc, text, -1, &trc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectFont(hdc, old_f);
  }
}

void DrawBadgePill(Gdiplus::Graphics& g, HDC hdc, const RECT& rc,
                   const wchar_t* text, HFONT font, COLORREF tint,
                   const Palette& pal, int dpi) {
  const float x = static_cast<float>(rc.left);
  const float y = static_cast<float>(rc.top);
  const float w = static_cast<float>(rc.right - rc.left);
  const float h = static_cast<float>(rc.bottom - rc.top);
  Gdiplus::GraphicsPath pill;
  BuildRoundedPath(pill, x, y, w, h, h * 0.5f);

  Gdiplus::SolidBrush fill(
      ToGdip(tint, pal.dark ? static_cast<BYTE>(48) : static_cast<BYTE>(30)));
  g.FillPath(&fill, &pill);

  Gdiplus::Pen rim(
      ToGdip(tint, pal.dark ? static_cast<BYTE>(115) : static_cast<BYTE>(85)),
      ScaleF(1.0f, dpi));
  g.DrawPath(&rim, &pill);

  if (text != nullptr) {
    RECT trc = rc;
    SetBkMode(hdc, TRANSPARENT);
    HFONT old_f = SelectFont(hdc, font);
    SetTextColor(hdc, tint);
    DrawTextW(hdc, text, -1, &trc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectFont(hdc, old_f);
  }
}

void DrawStatusOrb(Gdiplus::Graphics& g, float cx, float cy, float r,
                   OrbKind kind, const Palette& pal, int dpi) {
  COLORREF base = pal.accent;
  if (kind == OrbKind::kSuccess) base = pal.green;
  else if (kind == OrbKind::kWarning) base = pal.amber;
  else if (kind == OrbKind::kError) base = pal.red;

  // 柔和发光光晕
  const float halo = r * 1.24f;
  Gdiplus::SolidBrush halo_br(ToGdip(base, pal.dark ? 44 : 28));
  g.FillEllipse(&halo_br, cx - halo, cy - halo, halo * 2.0f, halo * 2.0f);

  // 立体渐变球体
  const Gdiplus::Color top_col(
      255, static_cast<BYTE>(std::min(255, GetRValue(base) + 28)),
      static_cast<BYTE>(std::min(255, GetGValue(base) + 28)),
      static_cast<BYTE>(std::min(255, GetBValue(base) + 28)));
  Gdiplus::LinearGradientBrush orb_br(Gdiplus::PointF(cx, cy - r),
                                      Gdiplus::PointF(cx, cy + r), top_col,
                                      ToGdip(base));
  g.FillEllipse(&orb_br, cx - r, cy - r, r * 2.0f, r * 2.0f);

  // 顶部微高光
  Gdiplus::Pen rim(Gdiplus::Color(85, 255, 255, 255), ScaleF(1.0f, dpi));
  g.DrawEllipse(&rim, cx - r, cy - r, r * 2.0f, r * 2.0f);

  // 内部白色精致矢量符号
  const float stroke = std::max(1.8f, r * 0.145f);
  Gdiplus::Pen sym(Gdiplus::Color(255, 255, 255, 255), stroke);
  sym.SetStartCap(Gdiplus::LineCapRound);
  sym.SetEndCap(Gdiplus::LineCapRound);
  sym.SetLineJoin(Gdiplus::LineJoinRound);
  Gdiplus::SolidBrush dot(Gdiplus::Color(255, 255, 255, 255));

  if (kind == OrbKind::kSuccess) {
    const Gdiplus::PointF pts[3] = {{cx - r * 0.38f, cy + r * 0.02f},
                                    {cx - r * 0.08f, cy + r * 0.32f},
                                    {cx + r * 0.40f, cy - r * 0.28f}};
    g.DrawLines(&sym, pts, 3);
  } else if (kind == OrbKind::kUpdate) {
    g.DrawLine(&sym, cx, cy - r * 0.38f, cx, cy + r * 0.32f);
    const Gdiplus::PointF arrow[3] = {{cx - r * 0.30f, cy + r * 0.04f},
                                      {cx, cy + r * 0.36f},
                                      {cx + r * 0.30f, cy + r * 0.04f}};
    g.DrawLines(&sym, arrow, 3);
  } else if (kind == OrbKind::kInfo) {
    const float dr = stroke * 0.65f;
    g.FillEllipse(&dot, cx - dr, cy - r * 0.38f - dr, dr * 2.0f, dr * 2.0f);
    g.DrawLine(&sym, cx, cy - r * 0.08f, cx, cy + r * 0.38f);
  } else if (kind == OrbKind::kWarning) {
    g.DrawLine(&sym, cx, cy - r * 0.36f, cx, cy + r * 0.08f);
    const float dr = stroke * 0.68f;
    g.FillEllipse(&dot, cx - dr, cy + r * 0.34f - dr, dr * 2.0f, dr * 2.0f);
  } else {
    const float cr = r * 0.32f;
    g.DrawLine(&sym, cx - cr, cy - cr, cx + cr, cy + cr);
    g.DrawLine(&sym, cx + cr, cy - cr, cx - cr, cy + cr);
  }
}

void DrawProgressBar(Gdiplus::Graphics& g, float x, float y, float w, float h,
                     float ratio, bool indeterminate, float anim_phase,
                     const Palette& pal, int dpi) {
  if (w <= 4.0f || h <= 2.0f) return;
  const float r = h * 0.5f;
  Gdiplus::GraphicsPath track;
  BuildRoundedPath(track, x, y, w, h, r);

  Gdiplus::SolidBrush track_br(
      pal.dark ? Gdiplus::Color(255, 66, 70, 82)
               : Gdiplus::Color(255, 220, 226, 236));
  g.FillPath(&track_br, &track);

  Gdiplus::GraphicsState st = g.Save();
  g.SetClip(&track);

  if (indeterminate) {
    const float seg_w = w * 0.36f;
    const float travel = w + seg_w;
    const float pos = std::fmod(std::max(0.0f, anim_phase), 1.0f);
    const float sx = x - seg_w + travel * pos;
    Gdiplus::GraphicsPath seg;
    BuildRoundedPath(seg, sx, y, seg_w, h, r);
    Gdiplus::LinearGradientBrush seg_br(
        Gdiplus::PointF(sx, y), Gdiplus::PointF(sx + seg_w, y),
        Gdiplus::Color(110, 0, 140, 255), Gdiplus::Color(255, 0, 200, 255));
    g.FillPath(&seg_br, &seg);
  } else {
    const float clamped = std::clamp(ratio, 0.0f, 1.0f);
    const float fw = w * clamped;
    if (fw > 1.0f) {
      Gdiplus::GraphicsPath fill;
      BuildRoundedPath(fill, x, y, std::max(h, fw), h, r);
      Gdiplus::LinearGradientBrush fill_br(
          Gdiplus::PointF(x, y), Gdiplus::PointF(x + std::max(h, fw), y),
          ToGdip(pal.accent), Gdiplus::Color(255, 0, 198, 255));
      g.FillPath(&fill_br, &fill);
    }
  }
  g.Restore(st);

  Gdiplus::Pen rim(
      pal.dark ? Gdiplus::Color(38, 255, 255, 255)
               : Gdiplus::Color(40, 0, 0, 0),
      ScaleF(0.8f, dpi));
  g.DrawPath(&rim, &track);
}

void ShowAlert(HWND parent, const std::wstring& title,
               const std::wstring& message, OrbKind kind,
               const std::wstring& button_text) {
  const GdiplusSession gdip;

  WNDCLASSEXW wc = {sizeof(wc)};
  wc.lpfnWndProc = AlertWndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = kAlertClassName;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  RegisterClassExW(&wc);

  POINT pt = {};
  GetCursorPos(&pt);
  const int dpi = (parent != nullptr && IsWindow(parent) != FALSE)
                      ? GetWindowDpi(parent)
                      : GetMonitorDpiForPoint(pt);
  const RECT work = GetMonitorWorkAreaForPoint(pt);

  AlertState state;
  state.title = title;
  state.message = message;
  state.button_text = button_text.empty() ? L"好" : button_text;
  state.kind = kind;
  state.pal = GetPalette(!IsSystemInLightTheme());
  RebuildAlertFonts(&state, dpi);

  // 根据文本行数动态估算窗口高度，彻底避免内容裁切
  const int win_w = Scale(364, dpi);
  int body_h = Scale(52, dpi);
  {
    HDC tmp = GetDC(nullptr);
    HFONT old_f = SelectFont(tmp, state.font_body);
    RECT calc = {0, 0, win_w - Scale(72, dpi), 0};
    DrawTextW(tmp, message.c_str(), -1, &calc,
              DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    body_h = std::max(Scale(44, dpi), static_cast<int>(calc.bottom - calc.top));
    SelectFont(tmp, old_f);
    ReleaseDC(nullptr, tmp);
  }
  const int win_h = std::clamp(Scale(226, dpi) + body_h, Scale(268, dpi),
                               Scale(440, dpi));

  const int x = work.left + ((work.right - work.left) - win_w) / 2;
  const int y = work.top + ((work.bottom - work.top) - win_h) / 2;

  const HWND owner =
      (parent != nullptr && IsWindow(parent) != FALSE) ? parent : nullptr;
  const bool disable_owner =
      (owner != nullptr && IsWindowVisible(owner) != FALSE);
  if (disable_owner) EnableWindow(owner, FALSE);

  HWND hwnd = CreateWindowExW(
      WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kAlertClassName, title.c_str(),
      WS_POPUP, x, y, win_w, win_h, owner, nullptr, GetModuleHandleW(nullptr),
      &state);

  if (hwnd != nullptr) {
    ApplyWindowChrome(hwnd, state.pal, true);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);

    MSG msg = {};
    while (!state.done && IsWindow(hwnd) != FALSE &&
           GetMessageW(&msg, nullptr, 0, 0) > 0) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (msg.message == WM_QUIT) {
      PostQuitMessage(static_cast<int>(msg.wParam));
    }
  }

  if (disable_owner && IsWindow(owner) != FALSE) {
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
  }

  if (state.font_title) DeleteObject(state.font_title);
  if (state.font_body) DeleteObject(state.font_body);
  if (state.font_btn) DeleteObject(state.font_btn);
  if (state.font_small) DeleteObject(state.font_small);
}

}  // namespace liquid_glass
