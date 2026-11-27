#include "./about_window.h"

#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <string>

#include "./config.h"
#include "./version.h"

namespace {
constexpr wchar_t kAboutClassName[] = L"CrossPointerAboutClass";
constexpr wchar_t kRepoUrl[] = L"https://github.com/Crucialmer/CrossPointer";

bool HitRect(const RECT& r, int x, int y) noexcept {
  return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

void DrawFeatureDotIcon(Gdiplus::Graphics& g, int index, float cx, float cy,
                        float r, const liquid_glass::Palette& pal, int dpi) {
  using namespace liquid_glass;
  const COLORREF tints[4] = {pal.accent, pal.green, RGB(142, 94, 255),
                             RGB(14, 165, 233)};
  const COLORREF tint = tints[index % 4];

  Gdiplus::SolidBrush halo(ToGdip(tint, pal.dark ? 52 : 32));
  g.FillEllipse(&halo, cx - r, cy - r, r * 2.0f, r * 2.0f);

  const float ir = r * 0.45f;
  Gdiplus::Pen pen(ToGdip(tint), std::max(1.3f, ScaleF(1.4f, dpi)));
  pen.SetStartCap(Gdiplus::LineCapRound);
  pen.SetEndCap(Gdiplus::LineCapRound);
  Gdiplus::SolidBrush dot(ToGdip(tint));

  if (index == 0) {
    // 240Hz lightning / pulse
    const Gdiplus::PointF pts[4] = {{cx + ir * 0.1f, cy - ir},
                                    {cx - ir * 0.6f, cy + ir * 0.1f},
                                    {cx + ir * 0.1f, cy + ir * 0.1f},
                                    {cx - ir * 0.15f, cy + ir}};
    g.DrawLines(&pen, pts, 4);
  } else if (index == 1) {
    // 8K crisp frame
    Gdiplus::GraphicsPath fp;
    BuildRoundedPath(fp, cx - ir, cy - ir * 0.78f, ir * 2.0f, ir * 1.56f,
                     ir * 0.3f);
    g.DrawPath(&pen, &fp);
    g.FillEllipse(&dot, cx - ir * 0.25f, cy - ir * 0.25f, ir * 0.5f, ir * 0.5f);
  } else if (index == 2) {
    // Single hotkey circle + dot
    g.DrawEllipse(&pen, cx - ir * 0.9f, cy - ir * 0.9f, ir * 1.8f, ir * 1.8f);
    g.FillEllipse(&dot, cx - ir * 0.34f, cy - ir * 0.34f, ir * 0.68f,
                  ir * 0.68f);
  } else {
    // Pure green leaf / check
    const Gdiplus::PointF ck[3] = {{cx - ir * 0.65f, cy + ir * 0.05f},
                                   {cx - ir * 0.12f, cy + ir * 0.58f},
                                   {cx + ir * 0.72f, cy - ir * 0.48f}};
    g.DrawLines(&pen, ck, 3);
  }
}
}  // namespace

AboutWindow::~AboutWindow() { Close(); }

void AboutWindow::Close() noexcept {
  if (hwnd_ != nullptr) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
  if (font_caption_) { DeleteObject(font_caption_); font_caption_ = nullptr; }
  if (font_app_) { DeleteObject(font_app_); font_app_ = nullptr; }
  if (font_badge_) { DeleteObject(font_badge_); font_badge_ = nullptr; }
  if (font_sub_) { DeleteObject(font_sub_); font_sub_ = nullptr; }
  if (font_feat_bold_) { DeleteObject(font_feat_bold_); font_feat_bold_ = nullptr; }
  if (font_feat_) { DeleteObject(font_feat_); font_feat_ = nullptr; }
  if (font_btn_) { DeleteObject(font_btn_); font_btn_ = nullptr; }
  gdip_.reset();
}

void AboutWindow::RebuildFonts(int dpi) {
  using namespace liquid_glass;
  if (font_caption_) DeleteObject(font_caption_);
  if (font_app_) DeleteObject(font_app_);
  if (font_badge_) DeleteObject(font_badge_);
  if (font_sub_) DeleteObject(font_sub_);
  if (font_feat_bold_) DeleteObject(font_feat_bold_);
  if (font_feat_) DeleteObject(font_feat_);
  if (font_btn_) DeleteObject(font_btn_);

  font_caption_ = CreateModernFont(86, FW_NORMAL, dpi);
  font_app_ = CreateModernFont(155, FW_BOLD, dpi);
  font_badge_ = CreateModernFont(85, FW_SEMIBOLD, dpi);
  font_sub_ = CreateModernFont(86, FW_NORMAL, dpi);
  font_feat_bold_ = CreateModernFont(90, FW_SEMIBOLD, dpi);
  font_feat_ = CreateModernFont(85, FW_NORMAL, dpi);
  font_btn_ = CreateModernFont(95, FW_SEMIBOLD, dpi);
}

void AboutWindow::Show(HINSTANCE instance, HWND parent) {
  if (hwnd_ != nullptr) {
    ShowWindow(hwnd_, SW_RESTORE);
    SetForegroundWindow(hwnd_);
    return;
  }

  instance_ = instance;
  parent_ = parent;
  pal_ = liquid_glass::GetPalette(!IsSystemInLightTheme());
  gdip_ = std::make_unique<liquid_glass::GdiplusSession>();
  hover_close_ = press_close_ = hover_link_ = hover_ok_ = press_ok_ = false;
  tracking_mouse_ = false;

  WNDCLASSEXW wc = {sizeof(wc)};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = instance_;
  wc.lpszClassName = kAboutClassName;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(101));
  RegisterClassExW(&wc);

  POINT pt = {};
  GetCursorPos(&pt);
  const int dpi = liquid_glass::GetMonitorDpiForPoint(pt);
  const RECT work = liquid_glass::GetMonitorWorkAreaForPoint(pt);
  RebuildFonts(dpi);

  const int win_w = liquid_glass::Scale(364, dpi);
  const int win_h = liquid_glass::Scale(464, dpi);
  const int x = work.left + ((work.right - work.left) - win_w) / 2;
  const int y = work.top + ((work.bottom - work.top) - win_h) / 2;

  hwnd_ = CreateWindowExW(WS_EX_APPWINDOW, kAboutClassName,
                          L"关于 CrossPointer", WS_POPUP, x, y, win_w, win_h,
                          parent_, nullptr, instance_, this);
  if (hwnd_ == nullptr) {
    Close();
    return;
  }

  liquid_glass::ApplyWindowChrome(hwnd_, pal_, true);
  ShowWindow(hwnd_, SW_SHOW);
  UpdateWindow(hwnd_);
  SetForegroundWindow(hwnd_);
}

void AboutWindow::NotifyThemeChanged() noexcept {
  if (hwnd_ == nullptr) return;
  pal_ = liquid_glass::GetPalette(!IsSystemInLightTheme());
  liquid_glass::ApplyWindowChrome(hwnd_, pal_, true);
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void AboutWindow::Paint(HDC screen_hdc) {
  using namespace liquid_glass;
  RECT rc = {};
  GetClientRect(hwnd_, &rc);
  const int w = rc.right - rc.left;
  const int h = rc.bottom - rc.top;
  if (w <= 0 || h <= 0) return;

  const int dpi = GetWindowDpi(hwnd_);
  HDC hdc = CreateCompatibleDC(screen_hdc);
  HBITMAP bmp = CreateCompatibleBitmap(screen_hdc, w, h);
  HBITMAP old_bmp = static_cast<HBITMAP>(SelectObject(hdc, bmp));

  const int close_cx = Scale(20, dpi);
  const int close_cy = Scale(20, dpi);
  const int close_hit = Scale(11, dpi);
  rc_close_ = {close_cx - close_hit, close_cy - close_hit, close_cx + close_hit,
               close_cy + close_hit};

  const int card_x = Scale(20, dpi);
  const int card_y = Scale(194, dpi);
  const int card_w = w - card_x * 2;
  const int card_h = Scale(168, dpi);

  rc_link_ = {Scale(48, dpi), card_y + card_h + Scale(14, dpi),
              w - Scale(48, dpi), card_y + card_h + Scale(38, dpi)};

  const int btn_w = Scale(126, dpi);
  const int btn_h = Scale(34, dpi);
  rc_ok_ = {(w - btn_w) / 2, h - Scale(50, dpi), (w + btn_w) / 2,
            h - Scale(50, dpi) + btn_h};

  const int badge_w = Scale(70, dpi);
  const int badge_h = Scale(23, dpi);
  RECT badge_rc = {(w - badge_w) / 2, Scale(138, dpi), (w + badge_w) / 2,
                   Scale(138, dpi) + badge_h};

  {
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

    DrawWindowBackground(g, static_cast<float>(w), static_cast<float>(h), pal_,
                         dpi);
    DrawTrafficLightClose(g, static_cast<float>(close_cx),
                          static_cast<float>(close_cy), ScaleF(6.5f, dpi),
                          hover_close_, press_close_, dpi);

    // 居中英雄图标
    const float icon_s = ScaleF(58.0f, dpi);
    DrawAppSquircleIcon(g, (static_cast<float>(w) - icon_s) * 0.5f,
                        ScaleF(40.0f, dpi), icon_s, pal_, dpi);

    // 版本号胶囊徽标
    DrawBadgePill(g, hdc, badge_rc, app_version::kVersionTag, font_badge_,
                  pal_.accent, pal_, dpi);

    // Inset Liquid Glass 特性卡片
    DrawLiquidGlassCard(g, static_cast<float>(card_x),
                        static_cast<float>(card_y), static_cast<float>(card_w),
                        static_cast<float>(card_h), ScaleF(14.0f, dpi), pal_,
                        dpi);

    // 4 项特性精致圆标与分割线
    Gdiplus::Pen div_pen(ToGdip(pal_.divider), ScaleF(1.0f, dpi));
    const float row_h = static_cast<float>(card_h) / 4.0f;
    for (int i = 0; i < 4; ++i) {
      const float ry = static_cast<float>(card_y) + row_h * i;
      DrawFeatureDotIcon(g, i, static_cast<float>(card_x) + ScaleF(26.0f, dpi),
                         ry + row_h * 0.5f, ScaleF(10.5f, dpi), pal_, dpi);
      if (i > 0) {
        g.DrawLine(&div_pen, static_cast<float>(card_x + Scale(48, dpi)), ry,
                   static_cast<float>(card_x + card_w - Scale(14, dpi)), ry);
      }
    }

    // 底部确认胶囊按钮
    DrawPillButton(g, hdc, rc_ok_, L"好", font_btn_, PillStyle::kPrimary,
                   hover_ok_, press_ok_, true, pal_, dpi);
  }

  // 文字层
  SetBkMode(hdc, TRANSPARENT);
  SelectFont(hdc, font_caption_);
  SetTextColor(hdc, pal_.secondary);
  RECT cap_rc = {Scale(44, dpi), Scale(10, dpi), w - Scale(44, dpi),
                 Scale(30, dpi)};
  DrawTextW(hdc, L"关于软件", -1, &cap_rc,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

  SelectFont(hdc, font_app_);
  SetTextColor(hdc, pal_.text);
  RECT app_rc = {Scale(20, dpi), Scale(104, dpi), w - Scale(20, dpi),
                 Scale(134, dpi)};
  DrawTextW(hdc, L"CrossPointer", -1, &app_rc,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

  SelectFont(hdc, font_sub_);
  SetTextColor(hdc, pal_.secondary);
  RECT sub_rc = {Scale(20, dpi), Scale(166, dpi), w - Scale(20, dpi),
                 Scale(188, dpi)};
  DrawTextW(hdc, L"极简屏幕十字辅助线 · macOS 26 Liquid Glass", -1, &sub_rc,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

  struct FeatureRow {
    const wchar_t* title;
    const wchar_t* desc;
  };
  const FeatureRow rows[4] = {
      {L"240Hz 零阻塞", L" · Direct2D 加速 · 待机 0% 唤醒"},
      {L"8K 高分屏适配", L" · PerMonitorV2 无损缩放"},
      {L"单键智能复用", L" · 无序修饰键 · 极速呼出退出"},
      {L"100% 纯绿色", L" · 原生 C++20 · 零外部残留"},
  };
  const int row_h_i = card_h / 4;
  for (int i = 0; i < 4; ++i) {
    const int ry = card_y + row_h_i * i;
    SelectFont(hdc, font_feat_bold_);
    SetTextColor(hdc, pal_.text);
    RECT trc = {card_x + Scale(48, dpi), ry, card_x + Scale(142, dpi),
                ry + row_h_i};
    DrawTextW(hdc, rows[i].title, -1, &trc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    SIZE sz = {};
    GetTextExtentPoint32W(hdc, rows[i].title,
                          static_cast<int>(wcslen(rows[i].title)), &sz);

    SelectFont(hdc, font_feat_);
    SetTextColor(hdc, pal_.secondary);
    RECT drc = {card_x + Scale(48, dpi) + sz.cx, ry,
                card_x + card_w - Scale(12, dpi), ry + row_h_i};
    DrawTextW(hdc, rows[i].desc, -1, &drc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  }

  // GitHub 链接 + 版权
  SelectFont(hdc, font_sub_);
  SetTextColor(hdc, hover_link_ ? pal_.accent : pal_.secondary);
  DrawTextW(hdc, L"GitHub: Crucialmer/CrossPointer  ·  MIT License", -1,
            &rc_link_, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

  BitBlt(screen_hdc, 0, 0, w, h, hdc, 0, 0, SRCCOPY);
  SelectObject(hdc, old_bmp);
  DeleteObject(bmp);
  DeleteDC(hdc);
}

LRESULT CALLBACK AboutWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp,
                                      LPARAM lp) noexcept {
  auto* self =
      reinterpret_cast<AboutWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_NCCREATE) {
    const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lp);
    self = static_cast<AboutWindow*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    return DefWindowProcW(hwnd, msg, wp, lp);
  }
  if (self != nullptr) {
    try {
      return self->HandleMessage(hwnd, msg, wp, lp);
    } catch (...) {
      return 0;
    }
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT AboutWindow::HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_ERASEBKGND:
      return 1;

    case WM_SETTINGCHANGE: {
      NotifyThemeChanged();
      return 0;
    }

    case WM_DPICHANGED: {
      const int dpi = LOWORD(wp);
      const auto* prc = reinterpret_cast<const RECT*>(lp);
      SetWindowPos(hwnd, nullptr, prc->left, prc->top, prc->right - prc->left,
                   prc->bottom - prc->top, SWP_NOZORDER | SWP_NOACTIVATE);
      RebuildFonts(dpi);
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }

    case WM_KEYDOWN: {
      if (wp == VK_ESCAPE || wp == VK_RETURN || wp == VK_SPACE) {
        Close();
        return 0;
      }
      break;
    }

    case WM_MOUSEMOVE: {
      if (!tracking_mouse_) {
        TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
        tracking_mouse_ = true;
      }
      const int mx = GET_X_LPARAM(lp);
      const int my = GET_Y_LPARAM(lp);
      const bool hc = HitRect(rc_close_, mx, my);
      const bool hl = HitRect(rc_link_, mx, my);
      const bool ho = HitRect(rc_ok_, mx, my);
      if (hc != hover_close_ || hl != hover_link_ || ho != hover_ok_) {
        hover_close_ = hc;
        hover_link_ = hl;
        hover_ok_ = ho;
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    }

    case WM_MOUSELEAVE: {
      tracking_mouse_ = false;
      if (hover_close_ || hover_link_ || hover_ok_) {
        hover_close_ = hover_link_ = hover_ok_ = false;
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    }

    case WM_SETCURSOR: {
      if (hover_close_ || hover_link_ || hover_ok_) {
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
      }
      break;
    }

    case WM_LBUTTONDOWN: {
      const int mx = GET_X_LPARAM(lp);
      const int my = GET_Y_LPARAM(lp);
      if (HitRect(rc_close_, mx, my)) {
        press_close_ = true;
        SetCapture(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      if (HitRect(rc_ok_, mx, my)) {
        press_ok_ = true;
        SetCapture(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      if (HitRect(rc_link_, mx, my)) {
        ShellExecuteW(nullptr, L"open", kRepoUrl, nullptr, nullptr, SW_SHOW);
        return 0;
      }
      ReleaseCapture();
      SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
      return 0;
    }

    case WM_LBUTTONUP: {
      const int mx = GET_X_LPARAM(lp);
      const int my = GET_Y_LPARAM(lp);
      if (GetCapture() == hwnd) ReleaseCapture();
      const bool was_close = press_close_;
      const bool was_ok = press_ok_;
      press_close_ = press_ok_ = false;
      if ((was_close && HitRect(rc_close_, mx, my)) ||
          (was_ok && HitRect(rc_ok_, mx, my))) {
        Close();
        return 0;
      }
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }

    case WM_PAINT: {
      PAINTSTRUCT ps = {};
      HDC hdc = BeginPaint(hwnd, &ps);
      Paint(hdc);
      EndPaint(hwnd, &ps);
      return 0;
    }

    case WM_CLOSE: {
      Close();
      return 0;
    }

    case WM_DESTROY: {
      hwnd_ = nullptr;
      return 0;
    }
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}
