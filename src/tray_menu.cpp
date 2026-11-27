#include "./tray_menu.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>

#include "./config.h"
#include "./tray.h"
#include "./version.h"

namespace {
constexpr wchar_t kTrayMenuClassName[] = L"CrossPointerTrayMenuClass";

void DrawMenuGlyph(Gdiplus::Graphics& g, int kind, float cx, float cy,
                   float size, Gdiplus::Color col, int dpi) {
  using namespace liquid_glass;
  const float stroke = std::max(1.25f, ScaleF(1.35f, dpi));
  Gdiplus::Pen pen(col, stroke);
  pen.SetStartCap(Gdiplus::LineCapRound);
  pen.SetEndCap(Gdiplus::LineCapRound);
  pen.SetLineJoin(Gdiplus::LineJoinRound);
  Gdiplus::SolidBrush fill(col);
  const float r = size * 0.44f;

  switch (kind) {
    case 0: {  // Crosshair
      g.DrawEllipse(&pen, cx - r * 0.42f, cy - r * 0.42f, r * 0.84f, r * 0.84f);
      g.DrawLine(&pen, cx - r, cy, cx - r * 0.56f, cy);
      g.DrawLine(&pen, cx + r * 0.56f, cy, cx + r, cy);
      g.DrawLine(&pen, cx, cy - r, cx, cy - r * 0.56f);
      g.DrawLine(&pen, cx, cy + r * 0.56f, cx, cy + r);
      break;
    }
    case 1: {  // Settings sliders
      const float y1 = cy - r * 0.44f;
      const float y2 = cy + r * 0.44f;
      g.DrawLine(&pen, cx - r, y1, cx + r, y1);
      g.DrawLine(&pen, cx - r, y2, cx + r, y2);
      const float dr = r * 0.30f;
      g.FillEllipse(&fill, cx - r * 0.32f - dr, y1 - dr, dr * 2.0f, dr * 2.0f);
      g.FillEllipse(&fill, cx + r * 0.32f - dr, y2 - dr, dr * 2.0f, dr * 2.0f);
      break;
    }
    case 2: {  // Update download arrow
      g.DrawLine(&pen, cx, cy - r * 0.85f, cx, cy + r * 0.45f);
      const Gdiplus::PointF arr[3] = {{cx - r * 0.48f, cy + r * 0.05f},
                                      {cx, cy + r * 0.52f},
                                      {cx + r * 0.48f, cy + r * 0.05f}};
      g.DrawLines(&pen, arr, 3);
      g.DrawLine(&pen, cx - r * 0.72f, cy + r * 0.88f, cx + r * 0.72f,
                 cy + r * 0.88f);
      break;
    }
    case 3: {  // Reload circular arc
      g.DrawArc(&pen, cx - r * 0.82f, cy - r * 0.82f, r * 1.64f, r * 1.64f,
                -60.0f, 290.0f);
      const Gdiplus::PointF tip[3] = {{cx + r * 0.16f, cy - r * 0.96f},
                                      {cx + r * 0.62f, cy - r * 0.66f},
                                      {cx + r * 0.24f, cy - r * 0.34f}};
      g.DrawLines(&pen, tip, 3);
      break;
    }
    case 4: {  // Config document
      Gdiplus::GraphicsPath doc;
      BuildRoundedPath(doc, cx - r * 0.72f, cy - r * 0.88f, r * 1.44f,
                       r * 1.76f, r * 0.25f);
      g.DrawPath(&pen, &doc);
      g.DrawLine(&pen, cx - r * 0.34f, cy - r * 0.25f, cx + r * 0.34f,
                 cy - r * 0.25f);
      g.DrawLine(&pen, cx - r * 0.34f, cy + r * 0.25f, cx + r * 0.16f,
                 cy + r * 0.25f);
      break;
    }
    case 5: {  // About info circle
      g.DrawEllipse(&pen, cx - r * 0.88f, cy - r * 0.88f, r * 1.76f, r * 1.76f);
      const float dr = std::max(1.0f, r * 0.16f);
      g.FillEllipse(&fill, cx - dr, cy - r * 0.42f - dr, dr * 2.0f, dr * 2.0f);
      g.DrawLine(&pen, cx, cy - r * 0.08f, cx, cy + r * 0.44f);
      break;
    }
    default: {  // Power / Exit
      g.DrawArc(&pen, cx - r * 0.80f, cy - r * 0.74f, r * 1.60f, r * 1.60f,
                -52.0f, 284.0f);
      g.DrawLine(&pen, cx, cy - r * 0.92f, cx, cy - r * 0.12f);
      break;
    }
  }
}
}  // namespace

TrayMenuWindow::~TrayMenuWindow() { Close(); }

void TrayMenuWindow::Close() noexcept {
  if (hwnd_ != nullptr) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
  if (font_brand_) { DeleteObject(font_brand_); font_brand_ = nullptr; }
  if (font_status_) { DeleteObject(font_status_); font_status_ = nullptr; }
  if (font_item_) { DeleteObject(font_item_); font_item_ = nullptr; }
  if (font_item_bold_) { DeleteObject(font_item_bold_); font_item_bold_ = nullptr; }
  if (font_hint_) { DeleteObject(font_hint_); font_hint_ = nullptr; }
  gdip_.reset();
}

void TrayMenuWindow::RebuildFonts(int dpi) {
  using namespace liquid_glass;
  if (font_brand_) DeleteObject(font_brand_);
  if (font_status_) DeleteObject(font_status_);
  if (font_item_) DeleteObject(font_item_);
  if (font_item_bold_) DeleteObject(font_item_bold_);
  if (font_hint_) DeleteObject(font_hint_);

  font_brand_ = CreateModernFont(100, FW_SEMIBOLD, dpi);
  font_status_ = CreateModernFont(82, FW_NORMAL, dpi);
  font_item_ = CreateModernFont(92, FW_NORMAL, dpi);
  font_item_bold_ = CreateModernFont(92, FW_SEMIBOLD, dpi);
  font_hint_ = CreateModernFont(78, FW_NORMAL, dpi);
}

void TrayMenuWindow::ComputeLayout(int dpi, int* out_w, int* out_h) {
  using namespace liquid_glass;
  const int w = Scale(242, dpi);
  const int pad_x = Scale(8, dpi);
  int y = Scale(8, dpi);

  const int header_h = Scale(56, dpi);
  header_rect_ = {pad_x, y, w - pad_x, y + header_h};
  y += header_h + Scale(6, dpi);

  const int item_h = Scale(32, dpi);
  const int sep_h = Scale(11, dpi);

  for (auto& item : items_) {
    if (item.is_separator) {
      item.rect = {pad_x + Scale(6, dpi), y, w - pad_x - Scale(6, dpi),
                   y + sep_h};
      y += sep_h;
    } else {
      item.rect = {pad_x, y, w - pad_x, y + item_h};
      y += item_h;
    }
  }
  y += Scale(8, dpi);

  if (out_w != nullptr) *out_w = w;
  if (out_h != nullptr) *out_h = y;
}

void TrayMenuWindow::Show(HINSTANCE instance, HWND owner, POINT anchor_pt,
                          bool crosshair_visible,
                          const std::wstring& hotkey_hint) {
  Close();

  instance_ = instance;
  owner_ = owner;
  crosshair_visible_ = crosshair_visible;
  hotkey_hint_ = hotkey_hint;
  hover_index_ = -1;
  tracking_mouse_ = false;
  pal_ = liquid_glass::GetPalette(!IsSystemInLightTheme());
  gdip_ = std::make_unique<liquid_glass::GdiplusSession>();

  items_.clear();
  items_.push_back({false, IDM_TRAY_TOGGLE,
                    crosshair_visible ? L"隐藏十字辅助线" : L"显示十字辅助线",
                    hotkey_hint, IconKind::kCrosshair, false, false, {}});
  items_.push_back({true, 0, L"", L"", IconKind::kCrosshair, false, false, {}});
  items_.push_back({false, IDM_TRAY_SETTINGS, L"偏好设置...", L"",
                    IconKind::kSettings, false, true, {}});
  items_.push_back({false, IDM_TRAY_CHECK_UPDATE, L"检查更新...", L"",
                    IconKind::kUpdate, false, false, {}});
  items_.push_back({false, IDM_TRAY_RELOAD, L"重新载入配置", L"",
                    IconKind::kReload, false, false, {}});
  items_.push_back({false, IDM_TRAY_OPEN_CONFIG, L"打开配置文件", L"",
                    IconKind::kConfig, false, false, {}});
  items_.push_back({true, 0, L"", L"", IconKind::kCrosshair, false, false, {}});
  items_.push_back({false, IDM_TRAY_ABOUT, L"关于软件", L"", IconKind::kAbout,
                    false, false, {}});
  items_.push_back({false, IDM_TRAY_EXIT, L"退出", L"", IconKind::kExit, true,
                    false, {}});

  WNDCLASSEXW wc = {sizeof(wc)};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = instance_;
  wc.lpszClassName = kTrayMenuClassName;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  RegisterClassExW(&wc);

  const int dpi = liquid_glass::GetMonitorDpiForPoint(anchor_pt);
  RebuildFonts(dpi);

  int win_w = 0;
  int win_h = 0;
  ComputeLayout(dpi, &win_w, &win_h);

  const RECT work = liquid_glass::GetMonitorWorkAreaForPoint(anchor_pt);
  const int gap = liquid_glass::Scale(6, dpi);

  int x = anchor_pt.x;
  int y = anchor_pt.y - win_h - gap;
  if (y < work.top + gap) {
    y = anchor_pt.y + gap;
  }
  if (x + win_w > work.right - gap) {
    x = anchor_pt.x - win_w;
  }
  x = std::clamp(x, static_cast<int>(work.left) + gap,
                 std::max<int>(work.left + gap, work.right - win_w - gap));
  y = std::clamp(y, static_cast<int>(work.top) + gap,
                 std::max<int>(work.top + gap, work.bottom - win_h - gap));

  hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kTrayMenuClassName,
                          L"CrossPointerMenu", WS_POPUP, x, y, win_w, win_h,
                          owner_, nullptr, instance_, this);
  if (hwnd_ == nullptr) {
    Close();
    return;
  }

  liquid_glass::ApplyWindowChrome(hwnd_, pal_, true);
  ShowWindow(hwnd_, SW_SHOW);
  UpdateWindow(hwnd_);
  SetForegroundWindow(hwnd_);
}

int TrayMenuWindow::HitTestItem(int x, int y) const noexcept {
  for (size_t i = 0; i < items_.size(); ++i) {
    if (items_[i].is_separator) continue;
    const RECT& r = items_[i].rect;
    if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void TrayMenuWindow::MoveSelection(int delta) {
  if (items_.empty()) return;
  const int count = static_cast<int>(items_.size());
  int idx = hover_index_;
  for (int step = 0; step < count; ++step) {
    if (idx < 0) {
      idx = (delta > 0) ? 0 : (count - 1);
    } else {
      idx = (idx + delta + count) % count;
    }
    if (!items_[static_cast<size_t>(idx)].is_separator) {
      hover_index_ = idx;
      InvalidateRect(hwnd_, nullptr, FALSE);
      return;
    }
  }
}

void TrayMenuWindow::TriggerItem(int index) {
  if (index < 0 || index >= static_cast<int>(items_.size())) return;
  const MenuItem& item = items_[static_cast<size_t>(index)];
  if (item.is_separator || item.command_id == 0) return;

  const UINT cmd = item.command_id;
  const HWND target = owner_;
  Close();
  if (target != nullptr && IsWindow(target) != FALSE) {
    PostMessageW(target, WM_COMMAND, cmd, 0);
  }
}

void TrayMenuWindow::Paint(HDC screen_hdc) {
  using namespace liquid_glass;
  RECT client = {};
  GetClientRect(hwnd_, &client);
  const int w = client.right - client.left;
  const int h = client.bottom - client.top;
  if (w <= 0 || h <= 0) return;

  const int dpi = GetWindowDpi(hwnd_);
  HDC hdc = CreateCompatibleDC(screen_hdc);
  HBITMAP bmp = CreateCompatibleBitmap(screen_hdc, w, h);
  HBITMAP old_bmp = static_cast<HBITMAP>(SelectObject(hdc, bmp));

  {
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

    // 1. 外层液态玻璃背景
    DrawWindowBackground(g, static_cast<float>(w), static_cast<float>(h), pal_,
                         dpi);

    // 2. 顶部同心液态玻璃状态卡片
    const float hx = static_cast<float>(header_rect_.left);
    const float hy = static_cast<float>(header_rect_.top);
    const float hw = static_cast<float>(header_rect_.right - header_rect_.left);
    const float hh = static_cast<float>(header_rect_.bottom - header_rect_.top);
    DrawLiquidGlassCard(g, hx, hy, hw, hh, ScaleF(10.0f, dpi), pal_, dpi);

    // 左上角微缩 Squircle 图标
    DrawAppSquircleIcon(g, hx + ScaleF(10.0f, dpi), hy + ScaleF(8.0f, dpi),
                        ScaleF(22.0f, dpi), pal_, dpi);

    // 呼吸光晕状态指示点
    const COLORREF dot_col = crosshair_visible_ ? pal_.green : pal_.accent;
    const float dx = hx + ScaleF(18.0f, dpi);
    const float dy = hy + ScaleF(41.5f, dpi);
    const float halo_r = ScaleF(5.2f, dpi);
    const float dot_r = ScaleF(3.0f, dpi);
    Gdiplus::SolidBrush halo_br(ToGdip(dot_col, pal_.dark ? 65 : 42));
    g.FillEllipse(&halo_br, dx - halo_r, dy - halo_r, halo_r * 2.0f,
                  halo_r * 2.0f);
    Gdiplus::SolidBrush dot_br(ToGdip(dot_col));
    g.FillEllipse(&dot_br, dx - dot_r, dy - dot_r, dot_r * 2.0f, dot_r * 2.0f);

    // 3. 菜单项悬停胶囊 + 矢量图标 + 细分割线
    Gdiplus::Pen sep_pen(ToGdip(pal_.divider), ScaleF(1.0f, dpi));
    for (size_t i = 0; i < items_.size(); ++i) {
      const MenuItem& item = items_[i];
      const RECT& r = item.rect;

      if (item.is_separator) {
        const float sy = static_cast<float>(r.top + r.bottom) * 0.5f;
        g.DrawLine(&sep_pen, static_cast<float>(r.left), sy,
                   static_cast<float>(r.right), sy);
        continue;
      }

      const bool hovered = (static_cast<int>(i) == hover_index_);
      if (hovered) {
        Gdiplus::GraphicsPath hp;
        BuildRoundedPath(hp, static_cast<float>(r.left),
                         static_cast<float>(r.top) + ScaleF(1.0f, dpi),
                         static_cast<float>(r.right - r.left),
                         static_cast<float>(r.bottom - r.top) -
                             ScaleF(2.0f, dpi),
                         ScaleF(7.5f, dpi));
        if (item.destructive) {
          Gdiplus::SolidBrush hb(ToGdip(pal_.red, pal_.dark ? 225 : 235));
          g.FillPath(&hb, &hp);
        } else {
          Gdiplus::LinearGradientBrush hb(
              Gdiplus::PointF(static_cast<float>(r.left),
                              static_cast<float>(r.top)),
              Gdiplus::PointF(static_cast<float>(r.left),
                              static_cast<float>(r.bottom)),
              Gdiplus::Color(255, 22, 138, 255),
              Gdiplus::Color(255, 0, 112, 236));
          g.FillPath(&hb, &hp);
        }
      }

      const Gdiplus::Color glyph_col =
          hovered ? Gdiplus::Color(255, 255, 255, 255)
                  : (item.destructive
                         ? ToGdip(pal_.red)
                         : (item.emphasized ? ToGdip(pal_.accent)
                                            : ToGdip(pal_.secondary)));
      DrawMenuGlyph(g, static_cast<int>(item.icon),
                    static_cast<float>(r.left) + ScaleF(18.0f, dpi),
                    static_cast<float>(r.top + r.bottom) * 0.5f,
                    ScaleF(13.5f, dpi), glyph_col, dpi);
    }
  }

  // 4. 文字层 (ClearType)
  SetBkMode(hdc, TRANSPARENT);

  // Header Brand + Version
  SelectFont(hdc, font_brand_);
  SetTextColor(hdc, pal_.text);
  RECT brand_rc = {header_rect_.left + Scale(38, dpi),
                   header_rect_.top + Scale(7, dpi),
                   header_rect_.right - Scale(46, dpi),
                   header_rect_.top + Scale(30, dpi)};
  DrawTextW(hdc, L"CrossPointer", -1, &brand_rc,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

  SelectFont(hdc, font_hint_);
  SetTextColor(hdc, pal_.secondary);
  RECT ver_rc = {header_rect_.right - Scale(48, dpi),
                 header_rect_.top + Scale(8, dpi),
                 header_rect_.right - Scale(10, dpi),
                 header_rect_.top + Scale(29, dpi)};
  DrawTextW(hdc, app_version::kVersionTag, -1, &ver_rc,
            DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

  SelectFont(hdc, font_status_);
  SetTextColor(hdc, pal_.secondary);
  RECT st_rc = {header_rect_.left + Scale(28, dpi),
                header_rect_.top + Scale(31, dpi),
                header_rect_.right - Scale(10, dpi),
                header_rect_.top + Scale(51, dpi)};
  DrawTextW(hdc,
            crosshair_visible_ ? L"十字辅助线已激活 · 1ms" : L"托盘静默待机中",
            -1, &st_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

  // Menu Item Labels & Shortcut Hints
  for (size_t i = 0; i < items_.size(); ++i) {
    const MenuItem& item = items_[i];
    if (item.is_separator) continue;
    const bool hovered = (static_cast<int>(i) == hover_index_);
    const RECT& r = item.rect;

    SelectFont(hdc, item.emphasized ? font_item_bold_ : font_item_);
    SetTextColor(hdc, hovered ? RGB(255, 255, 255)
                              : (item.destructive ? pal_.red : pal_.text));
    RECT lrc = {r.left + Scale(34, dpi), r.top, r.right - Scale(10, dpi),
                r.bottom};
    DrawTextW(hdc, item.label.c_str(), -1, &lrc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    if (!item.shortcut.empty()) {
      SelectFont(hdc, font_hint_);
      SetTextColor(hdc, hovered ? RGB(230, 242, 255) : pal_.secondary);
      RECT src = {r.left + Scale(120, dpi), r.top, r.right - Scale(10, dpi),
                  r.bottom};
      DrawTextW(hdc, item.shortcut.c_str(), -1, &src,
                DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
  }

  BitBlt(screen_hdc, 0, 0, w, h, hdc, 0, 0, SRCCOPY);
  SelectObject(hdc, old_bmp);
  DeleteObject(bmp);
  DeleteDC(hdc);
}

LRESULT CALLBACK TrayMenuWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp,
                                         LPARAM lp) noexcept {
  auto* self =
      reinterpret_cast<TrayMenuWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_NCCREATE) {
    const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lp);
    self = static_cast<TrayMenuWindow*>(cs->lpCreateParams);
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

LRESULT TrayMenuWindow::HandleMessage(HWND hwnd, UINT msg, WPARAM wp,
                                      LPARAM lp) {
  switch (msg) {
    case WM_ERASEBKGND:
      return 1;

    case WM_ACTIVATE: {
      if (LOWORD(wp) == WA_INACTIVE) {
        Close();
        return 0;
      }
      break;
    }

    case WM_KEYDOWN: {
      if (wp == VK_ESCAPE) {
        Close();
        return 0;
      }
      if (wp == VK_UP) {
        MoveSelection(-1);
        return 0;
      }
      if (wp == VK_DOWN) {
        MoveSelection(1);
        return 0;
      }
      if (wp == VK_RETURN && hover_index_ >= 0) {
        TriggerItem(hover_index_);
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
      const int idx = HitTestItem(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
      if (idx != hover_index_) {
        hover_index_ = idx;
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    }

    case WM_MOUSELEAVE: {
      tracking_mouse_ = false;
      if (hover_index_ != -1) {
        hover_index_ = -1;
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    }

    case WM_SETCURSOR: {
      if (hover_index_ != -1) {
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
      }
      break;
    }

    case WM_LBUTTONUP: {
      const int idx = HitTestItem(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
      if (idx >= 0) {
        TriggerItem(idx);
      }
      return 0;
    }

    case WM_PAINT: {
      PAINTSTRUCT ps = {};
      HDC hdc = BeginPaint(hwnd, &ps);
      Paint(hdc);
      EndPaint(hwnd, &ps);
      return 0;
    }

    case WM_DESTROY: {
      hwnd_ = nullptr;
      return 0;
    }
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}
