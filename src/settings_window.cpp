#include "./settings_window.h"

#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <objidl.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <vector>

// Windows SDK <gdiplus.h> requires unqualified min/max under NOMINMAX
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

#include "./autostart.h"
#include "./config_file_util.h"
#include "./tray.h"
#include "./version.h"

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
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif

namespace {
constexpr wchar_t kSettingsClassName[] = L"CrossPointerSettingsClass";
constexpr UINT_PTR kHoverSubclassId = 9001;
constexpr UINT_PTR kSliderSubclassId = 9002;

// Logical client dimensions (macOS 26 spacious Tahoe aesthetic)
constexpr int kClientW = 680;
constexpr int kClientH = 624;
constexpr int kPadX = 24;
constexpr int kCardW = 632;
constexpr int kSegY = 82;
constexpr int kSegH = 38;
constexpr int kCardY = 130;
constexpr int kCardH = 420;
constexpr int kFooterY = 566;
constexpr int kFooterH = 38;

// Tab 0: 外观尺寸
constexpr UINT IDC_SLIDER_GAP = 3010;
constexpr UINT IDC_LBL_GAP_VAL = 3011;
constexpr UINT IDC_SLIDER_H_WIDTH = 3012;
constexpr UINT IDC_LBL_H_WIDTH_VAL = 3013;
constexpr UINT IDC_SLIDER_V_WIDTH = 3014;
constexpr UINT IDC_LBL_V_WIDTH_VAL = 3015;
constexpr UINT IDC_PREVIEW_CROSSHAIR = 3016;

// Tab 1: 色彩模式
constexpr UINT IDC_SW_AUTO_THEME = 3020;
constexpr UINT IDC_PREVIEW_COLOR_A = 3021;
constexpr UINT IDC_BTN_COLOR_A = 3022;
constexpr UINT IDC_SLIDER_ALPHA_A = 3023;
constexpr UINT IDC_LBL_ALPHA_A_VAL = 3024;
constexpr UINT IDC_PREVIEW_COLOR_B = 3025;
constexpr UINT IDC_BTN_COLOR_B = 3026;
constexpr UINT IDC_SLIDER_ALPHA_B = 3027;
constexpr UINT IDC_LBL_ALPHA_B_VAL = 3028;

// Tab 2: 快捷键
constexpr UINT IDC_KEY_TOGGLE_CTRL = 3030;
constexpr UINT IDC_KEY_TOGGLE_ALT = 3031;
constexpr UINT IDC_KEY_TOGGLE_WIN = 3032;
constexpr UINT IDC_KEY_TOGGLE_SHIFT = 3033;
constexpr UINT IDC_COMBO_TOGGLE_VK = 3034;

constexpr UINT IDC_KEY_EXIT_CTRL = 3040;
constexpr UINT IDC_KEY_EXIT_ALT = 3041;
constexpr UINT IDC_KEY_EXIT_WIN = 3042;
constexpr UINT IDC_KEY_EXIT_SHIFT = 3043;
constexpr UINT IDC_COMBO_EXIT_VK = 3044;
constexpr UINT IDC_BTN_SYNC_HOTKEY = 3045;

// Tab 3: 系统与更新
constexpr UINT IDC_SW_SHOW_ON_STARTUP = 3049;
constexpr UINT IDC_SW_AUTO_DPI = 3050;
constexpr UINT IDC_SW_AUTOSTART = 3051;
constexpr UINT IDC_SW_AUTO_UPDATE = 3052;
constexpr UINT IDC_BTN_CHECK_UPDATE = 3053;

// 底部动作按钮
constexpr UINT IDC_BTN_SAVE = 3060;
constexpr UINT IDC_BTN_DEFAULTS = 3061;
constexpr UINT IDC_BTN_CLOSE = 3062;

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
  if (fn != nullptr) {
    if (fn(rc, style, FALSE, ex_style, static_cast<UINT>(dpi)) != FALSE) {
      return;
    }
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

Gdiplus::Color ToGdip(COLORREF c, BYTE alpha = 255) noexcept {
  return Gdiplus::Color(alpha, GetRValue(c), GetGValue(c), GetBValue(c));
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

// macOS 26 (Tahoe) Liquid Glass 表面绘制原语：柔和环境投影 + 光学垂直渐变 + 折射边框 + 顶部镜面高光
void DrawLiquidGlassCard(Gdiplus::Graphics& g, float x, float y, float w,
                         float h, float r, bool dark_mode, int dpi) {
  // 1. 柔和外层环境投影
  for (int i = 2; i >= 1; --i) {
    Gdiplus::GraphicsPath shadow_path;
    const float spread = ScaleF(static_cast<float>(i), dpi);
    BuildRoundedPath(shadow_path, x - spread * 0.4f, y + spread * 0.9f,
                     w + spread * 0.8f, h + spread * 0.6f, r + spread * 0.5f);
    const BYTE a = dark_mode ? static_cast<BYTE>(28 / i) : static_cast<BYTE>(14 / i);
    Gdiplus::SolidBrush shadow_br(Gdiplus::Color(a, 0, 0, 0));
    g.FillPath(&shadow_br, &shadow_path);
  }

  // 2. 光学垂直玻璃渐变主体
  Gdiplus::GraphicsPath card_path;
  BuildRoundedPath(card_path, x, y, w, h, r);
  const Gdiplus::Color top_col =
      dark_mode ? Gdiplus::Color(255, 48, 50, 56)
                : Gdiplus::Color(255, 255, 255, 255);
  const Gdiplus::Color bot_col =
      dark_mode ? Gdiplus::Color(255, 38, 40, 45)
                : Gdiplus::Color(255, 248, 250, 253);
  Gdiplus::LinearGradientBrush fill_br(
      Gdiplus::PointF(x, y), Gdiplus::PointF(x, y + h), top_col, bot_col);
  g.FillPath(&fill_br, &card_path);

  // 3. 顶部镜面高光反射 (Specular Top Sheen)
  Gdiplus::GraphicsState state = g.Save();
  g.SetClip(&card_path);
  Gdiplus::Pen spec_pen(
      dark_mode ? Gdiplus::Color(48, 255, 255, 255)
                : Gdiplus::Color(220, 255, 255, 255),
      ScaleF(1.25f, dpi));
  g.DrawLine(&spec_pen, x + r * 0.6f, y + ScaleF(1.0f, dpi),
             x + w - r * 0.6f, y + ScaleF(1.0f, dpi));
  g.Restore(state);

  // 4. 精细玻璃折射外边框
  Gdiplus::Pen border_pen(
      dark_mode ? Gdiplus::Color(62, 255, 255, 255)
                : Gdiplus::Color(255, 218, 223, 232),
      ScaleF(1.0f, dpi));
  g.DrawPath(&border_pen, &card_path);
}

// 绘制 SF-Symbols 风格的极简抗锯齿 Tab 矢量图标（彻底告别 GDI 彩色 Emoji 退化）
void DrawTabGlyph(Gdiplus::Graphics& g, int tab_index, float cx, float cy,
                  float size, Gdiplus::Color color, int dpi) {
  const float stroke = std::max(1.2f, ScaleF(1.45f, dpi));
  Gdiplus::Pen pen(color, stroke);
  pen.SetStartCap(Gdiplus::LineCapRound);
  pen.SetEndCap(Gdiplus::LineCapRound);
  pen.SetLineJoin(Gdiplus::LineJoinRound);
  const float r = size * 0.44f;

  if (tab_index == 0) {
    // 十字准星标
    g.DrawEllipse(&pen, cx - r * 0.45f, cy - r * 0.45f, r * 0.9f, r * 0.9f);
    g.DrawLine(&pen, cx - r, cy, cx - r * 0.55f, cy);
    g.DrawLine(&pen, cx + r * 0.55f, cy, cx + r, cy);
    g.DrawLine(&pen, cx, cy - r, cx, cy - r * 0.55f);
    g.DrawLine(&pen, cx, cy + r * 0.55f, cx, cy + r);
  } else if (tab_index == 1) {
    // 深浅对比圆环 (Theme Contrast)
    g.DrawEllipse(&pen, cx - r, cy - r, r * 2.0f, r * 2.0f);
    Gdiplus::SolidBrush fill(color);
    g.FillPie(&fill, cx - r * 0.78f, cy - r * 0.78f, r * 1.56f, r * 1.56f,
              -90.0f, 180.0f);
  } else if (tab_index == 2) {
    // 快捷键键帽 (Keycap)
    Gdiplus::GraphicsPath kp;
    BuildRoundedPath(kp, cx - r, cy - r * 0.76f, r * 2.0f, r * 1.52f, r * 0.38f);
    g.DrawPath(&pen, &kp);
    g.DrawLine(&pen, cx - r * 0.45f, cy - r * 0.15f, cx + r * 0.45f,
               cy - r * 0.15f);
    g.DrawLine(&pen, cx - r * 0.25f, cy + r * 0.28f, cx + r * 0.25f,
               cy + r * 0.28f);
  } else {
    // 精密滑杆 (System Sliders)
    const float y1 = cy - r * 0.45f;
    const float y2 = cy + r * 0.45f;
    g.DrawLine(&pen, cx - r, y1, cx + r, y1);
    g.DrawLine(&pen, cx - r, y2, cx + r, y2);
    Gdiplus::SolidBrush fill(color);
    const float dot = r * 0.32f;
    g.FillEllipse(&fill, cx - r * 0.35f - dot, y1 - dot, dot * 2.0f, dot * 2.0f);
    g.FillEllipse(&fill, cx + r * 0.35f - dot, y2 - dot, dot * 2.0f, dot * 2.0f);
  }
}

const std::vector<std::pair<std::wstring, unsigned int>>& GetKeyList() {
  static const std::vector<std::pair<std::wstring, unsigned int>> list = {
      {L"A", 'A'}, {L"B", 'B'}, {L"C", 'C'}, {L"D", 'D'}, {L"E", 'E'},
      {L"F", 'F'}, {L"G", 'G'}, {L"H", 'H'}, {L"I", 'I'}, {L"J", 'J'},
      {L"K", 'K'}, {L"L", 'L'}, {L"M", 'M'}, {L"N", 'N'}, {L"O", 'O'},
      {L"P", 'P'}, {L"Q", 'Q'}, {L"R", 'R'}, {L"S", 'S'}, {L"T", 'T'},
      {L"U", 'U'}, {L"V", 'V'}, {L"W", 'W'}, {L"X", 'X'}, {L"Y", 'Y'},
      {L"Z", 'Z'}, {L"0", '0'}, {L"1", '1'}, {L"2", '2'}, {L"3", '3'},
      {L"4", '4'}, {L"5", '5'}, {L"6", '6'}, {L"7", '7'}, {L"8", '8'},
      {L"9", '9'}, {L"F1", VK_F1}, {L"F2", VK_F2}, {L"F3", VK_F3},
      {L"F4", VK_F4}, {L"F5", VK_F5}, {L"F6", VK_F6}, {L"F7", VK_F7},
      {L"F8", VK_F8}, {L"F9", VK_F9}, {L"F10", VK_F10}, {L"F11", VK_F11},
      {L"F12", VK_F12}, {L"Space", VK_SPACE}, {L"Tab", VK_TAB},
      {L"Enter", VK_RETURN}, {L"Esc", VK_ESCAPE}, {L"Backspace", VK_BACK},
      {L"Delete", VK_DELETE}, {L"Insert", VK_INSERT},
      {L"Plus (+)", VK_OEM_PLUS}, {L"Minus (-)", VK_OEM_MINUS},
      {L"Comma (,)", VK_OEM_COMMA}, {L"Period (.)", VK_OEM_PERIOD},
      {L"Grave (`)", VK_OEM_3}};
  return list;
}

bool IsOwnerButtonControl(UINT id) noexcept {
  return id == IDC_BTN_SAVE || id == IDC_BTN_DEFAULTS || id == IDC_BTN_CLOSE ||
         id == IDC_BTN_COLOR_A || id == IDC_BTN_COLOR_B ||
         id == IDC_BTN_SYNC_HOTKEY || id == IDC_BTN_CHECK_UPDATE;
}

bool IsToggleSwitchControl(UINT id) noexcept {
  return id == IDC_SW_AUTO_THEME || id == IDC_SW_SHOW_ON_STARTUP ||
         id == IDC_SW_AUTO_DPI || id == IDC_SW_AUTOSTART ||
         id == IDC_SW_AUTO_UPDATE;
}

bool IsKeycapControl(UINT id) noexcept {
  return (id >= IDC_KEY_TOGGLE_CTRL && id <= IDC_KEY_TOGGLE_SHIFT) ||
         (id >= IDC_KEY_EXIT_CTRL && id <= IDC_KEY_EXIT_SHIFT);
}

bool IsValuePillControl(UINT id) noexcept {
  return id == IDC_LBL_GAP_VAL || id == IDC_LBL_H_WIDTH_VAL ||
         id == IDC_LBL_V_WIDTH_VAL || id == IDC_LBL_ALPHA_A_VAL ||
         id == IDC_LBL_ALPHA_B_VAL;
}
}  // namespace

SettingsWindow::~SettingsWindow() { Close(); }

void SettingsWindow::Close() noexcept {
  if (hwnd_ != nullptr) {
    KillTimer(hwnd_, kToastTimerId);
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
  hovered_ctrl_ = nullptr;
  toast_visible_ = false;

  if (font_title_ != nullptr) { DeleteObject(font_title_); font_title_ = nullptr; }
  if (font_card_title_ != nullptr) { DeleteObject(font_card_title_); font_card_title_ = nullptr; }
  if (font_bold_ != nullptr) { DeleteObject(font_bold_); font_bold_ = nullptr; }
  if (font_regular_ != nullptr) { DeleteObject(font_regular_); font_regular_ = nullptr; }
  if (font_small_ != nullptr) { DeleteObject(font_small_); font_small_ = nullptr; }
  if (br_bg_ != nullptr) { DeleteObject(br_bg_); br_bg_ = nullptr; }
  if (br_card_ != nullptr) { DeleteObject(br_card_); br_card_ = nullptr; }
  if (br_combo_ != nullptr) { DeleteObject(br_combo_); br_combo_ = nullptr; }

  gdip_.reset();
}

void SettingsWindow::Show(HINSTANCE instance, HWND parent,
                          const Config& current_config,
                          const std::wstring& config_path,
                          bool crosshair_visible, ApplyCallback on_apply) {
  if (hwnd_ != nullptr) {
    crosshair_visible_ = crosshair_visible;
    ShowWindow(hwnd_, SW_RESTORE);
    SetForegroundWindow(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
    return;
  }

  instance_ = instance;
  parent_ = parent;
  config_ = current_config;
  config_path_ = config_path;
  crosshair_visible_ = crosshair_visible;
  on_apply_ = std::move(on_apply);
  current_tab_ = 0;
  hover_tab_ = -1;
  hovered_ctrl_ = nullptr;
  toast_visible_ = false;

  temp_light_color_ = config_.light_theme;
  temp_dark_color_ = config_.dark_theme;
  temp_horiz_color_ = {config_.horizontal.r, config_.horizontal.g,
                       config_.horizontal.b, config_.horizontal.alpha};
  temp_vert_color_ = {config_.vertical.r, config_.vertical.g,
                      config_.vertical.b, config_.vertical.alpha};

  if (!gdip_) {
    gdip_ = std::make_unique<liquid_glass::GdiplusSession>();
  }

  INITCOMMONCONTROLSEX icex = {
      sizeof(icex),
      ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
  InitCommonControlsEx(&icex);

  WNDCLASSEXW wc = {sizeof(wc)};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = instance_;
  wc.lpszClassName = kSettingsClassName;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(101));
  RegisterClassExW(&wc);

  POINT cursor = {};
  GetCursorPos(&cursor);
  const int dpi = GetMonitorDpiForPoint(cursor);
  const HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi = {sizeof(mi)};
  RECT work_area = {0, 0, GetSystemMetrics(SM_CXSCREEN),
                    GetSystemMetrics(SM_CYSCREEN)};
  if (monitor != nullptr && GetMonitorInfoW(monitor, &mi) != FALSE) {
    work_area = mi.rcWork;
  }

  const DWORD style =
      WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
  const DWORD ex_style = WS_EX_APPWINDOW;
  RECT win_rc = {0, 0, Scale(kClientW, dpi), Scale(kClientH, dpi)};
  AdjustWindowRectForDpiCompat(&win_rc, style, ex_style, dpi);
  const int win_width = win_rc.right - win_rc.left;
  const int win_height = win_rc.bottom - win_rc.top;

  const int x =
      work_area.left + ((work_area.right - work_area.left) - win_width) / 2;
  const int y =
      work_area.top + ((work_area.bottom - work_area.top) - win_height) / 2;

  hwnd_ = CreateWindowExW(ex_style, kSettingsClassName, L"CrossPointer 偏好设置",
                          style, x, y, win_width, win_height, parent_, nullptr,
                          instance_, this);
  if (hwnd_ == nullptr) return;

  // macOS 26 质感：启用 DWM 原生圆角与无缝标题栏
  const DWMNCRENDERINGPOLICY ncp = DWMNCRP_ENABLED;
  DwmSetWindowAttribute(hwnd_, DWMWA_NCRENDERING_POLICY, &ncp, sizeof(ncp));
  const DWORD round_pref = DWMWCP_ROUND;
  DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &round_pref,
                        sizeof(round_pref));

  CreateControls();
  UpdateThemeStyles();
  LayoutControls(GetWindowDpi(hwnd_));
  LoadValuesToUI();
  UpdateTabVisibility();

  ShowWindow(hwnd_, SW_SHOW);
  UpdateWindow(hwnd_);
  SetForegroundWindow(hwnd_);
}

void SettingsWindow::NotifyThemeChanged() noexcept {
  if (hwnd_ == nullptr) return;
  UpdateThemeStyles();
  RedrawWindow(hwnd_, nullptr, nullptr,
               RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

void SettingsWindow::NotifyVisibilityChanged(bool crosshair_visible) noexcept {
  crosshair_visible_ = crosshair_visible;
  if (hwnd_ != nullptr) {
    InvalidateRect(hwnd_, nullptr, FALSE);
  }
}

void SettingsWindow::UpdateThemeStyles() {
  is_dark_mode_ = !IsSystemInLightTheme();

  if (br_bg_ != nullptr) DeleteObject(br_bg_);
  if (br_card_ != nullptr) DeleteObject(br_card_);
  if (br_combo_ != nullptr) DeleteObject(br_combo_);

  if (is_dark_mode_) {
    // macOS 26 深空液态黑曜石色板
    cr_bg_ = RGB(26, 27, 31);
    cr_card_ = RGB(43, 45, 50);
    cr_text_ = RGB(245, 246, 250);
    cr_secondary_ = RGB(154, 158, 168);
    cr_card_border_ = RGB(66, 70, 78);
    cr_divider_ = RGB(58, 60, 68);
    cr_accent_ = RGB(10, 132, 255);
    cr_green_ = RGB(48, 209, 88);
    br_combo_ = CreateSolidBrush(RGB(52, 54, 60));
  } else {
    // macOS 26 通透雾白液态玻璃色板
    cr_bg_ = RGB(241, 243, 247);
    cr_card_ = RGB(252, 253, 255);
    cr_text_ = RGB(26, 28, 32);
    cr_secondary_ = RGB(114, 118, 128);
    cr_card_border_ = RGB(218, 223, 232);
    cr_divider_ = RGB(230, 234, 241);
    cr_accent_ = RGB(0, 122, 255);
    cr_green_ = RGB(40, 185, 76);
    br_combo_ = CreateSolidBrush(RGB(255, 255, 255));
  }

  br_bg_ = CreateSolidBrush(cr_bg_);
  br_card_ = CreateSolidBrush(cr_card_);

  if (hwnd_ != nullptr) {
    const BOOL dark_attr = is_dark_mode_ ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark_attr,
                          sizeof(dark_attr));
    const COLORREF caption_color = cr_bg_;
    DwmSetWindowAttribute(hwnd_, DWMWA_CAPTION_COLOR, &caption_color,
                          sizeof(caption_color));
    const COLORREF border_color = cr_card_border_;
    DwmSetWindowAttribute(hwnd_, DWMWA_BORDER_COLOR, &border_color,
                          sizeof(border_color));
  }

  const wchar_t* combo_theme = is_dark_mode_ ? L"DarkMode_CFD" : L"Explorer";
  if (combo_toggle_vk_ != nullptr) {
    SetWindowTheme(combo_toggle_vk_, combo_theme, nullptr);
  }
  if (combo_exit_vk_ != nullptr) {
    SetWindowTheme(combo_exit_vk_, combo_theme, nullptr);
  }
}

LRESULT CALLBACK SettingsWindow::ControlHoverSubclassProc(
    HWND ctrl, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR subclass_id,
    DWORD_PTR ref_data) noexcept {
  auto* self = reinterpret_cast<SettingsWindow*>(ref_data);
  if (msg == WM_MOUSEMOVE && self != nullptr) {
    if (self->hovered_ctrl_ != ctrl) {
      self->hovered_ctrl_ = ctrl;
      TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, ctrl, 0};
      TrackMouseEvent(&tme);
      InvalidateRect(ctrl, nullptr, FALSE);
    }
  } else if (msg == WM_MOUSELEAVE && self != nullptr) {
    if (self->hovered_ctrl_ == ctrl) {
      self->hovered_ctrl_ = nullptr;
      InvalidateRect(ctrl, nullptr, FALSE);
    }
  } else if (msg == WM_SETCURSOR) {
    SetCursor(LoadCursorW(nullptr, IDC_HAND));
    return TRUE;
  } else if (msg == WM_ERASEBKGND) {
    return 1;
  } else if (msg == WM_NCDESTROY) {
    RemoveWindowSubclass(ctrl, ControlHoverSubclassProc, subclass_id);
  }
  return DefSubclassProc(ctrl, msg, wp, lp);
}

LRESULT CALLBACK SettingsWindow::SliderSubclassProc(
    HWND ctrl, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR subclass_id,
    DWORD_PTR ref_data) noexcept {
  auto* self = reinterpret_cast<SettingsWindow*>(ref_data);
  if (msg == WM_ERASEBKGND) {
    return 1;
  }
  if (msg == WM_MOUSEMOVE && self != nullptr) {
    if (self->hovered_ctrl_ != ctrl) {
      self->hovered_ctrl_ = ctrl;
      TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, ctrl, 0};
      TrackMouseEvent(&tme);
      InvalidateRect(ctrl, nullptr, FALSE);
    }
  } else if (msg == WM_MOUSELEAVE && self != nullptr) {
    if (self->hovered_ctrl_ == ctrl) {
      self->hovered_ctrl_ = nullptr;
      InvalidateRect(ctrl, nullptr, FALSE);
    }
  } else if (msg == WM_SETCURSOR) {
    SetCursor(LoadCursorW(nullptr, IDC_HAND));
    return TRUE;
  } else if (msg == WM_PAINT && self != nullptr) {
    PAINTSTRUCT ps = {};
    HDC hdc = BeginPaint(ctrl, &ps);
    try {
      self->PaintSliderControl(ctrl, hdc);
    } catch (...) {
    }
    EndPaint(ctrl, &ps);
    return 0;
  } else if (msg == WM_NCDESTROY) {
    RemoveWindowSubclass(ctrl, SliderSubclassProc, subclass_id);
  }
  return DefSubclassProc(ctrl, msg, wp, lp);
}

void SettingsWindow::CreateControls() {
  auto attach_hover = [&](HWND ctrl) {
    if (ctrl != nullptr) {
      SetWindowSubclass(ctrl, ControlHoverSubclassProc, kHoverSubclassId,
                        reinterpret_cast<DWORD_PTR>(this));
    }
  };

  auto create_owner_btn = [&](const wchar_t* text, UINT id,
                              bool visible = false) {
    const DWORD style = WS_CHILD | BS_OWNERDRAW | WS_TABSTOP |
                        (visible ? WS_VISIBLE : 0U);
    HWND h = CreateWindowExW(
        0, L"BUTTON", text, style, 0, 0, 10, 10, hwnd_,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), instance_, nullptr);
    attach_hover(h);
    return h;
  };

  auto create_slider = [&](UINT id, int min_val, int max_val) {
    HWND sl = CreateWindowExW(
        0, TRACKBAR_CLASSW, L"", WS_CHILD | TBS_NOTICKS | WS_TABSTOP, 0, 0, 10,
        10, hwnd_, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
        instance_, nullptr);
    SendMessageW(sl, TBM_SETRANGE, TRUE, MAKELPARAM(min_val, max_val));
    if (sl != nullptr) {
      SetWindowSubclass(sl, SliderSubclassProc, kSliderSubclassId,
                        reinterpret_cast<DWORD_PTR>(this));
    }
    return sl;
  };

  auto create_val_pill = [&](const wchar_t* text, UINT id) {
    return CreateWindowExW(
        0, L"STATIC", text, WS_CHILD | SS_OWNERDRAW, 0, 0, 10, 10, hwnd_,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), instance_, nullptr);
  };

  auto create_combo = [&](UINT id) {
    HWND cb = CreateWindowExW(
        0, L"COMBOBOX", L"", WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
        0, 0, 10, 10, hwnd_,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), instance_, nullptr);
    for (const auto& item : GetKeyList()) {
      SendMessageW(cb, CB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(item.first.c_str()));
    }
    return cb;
  };

  // Tab 0: 外观尺寸
  slider_gap_ = create_slider(IDC_SLIDER_GAP, 0, 120);
  lbl_gap_val_ = create_val_pill(L"16 px", IDC_LBL_GAP_VAL);
  slider_h_width_ = create_slider(IDC_SLIDER_H_WIDTH, 1, 32);
  lbl_h_width_val_ = create_val_pill(L"2 px", IDC_LBL_H_WIDTH_VAL);
  slider_v_width_ = create_slider(IDC_SLIDER_V_WIDTH, 1, 32);
  lbl_v_width_val_ = create_val_pill(L"2 px", IDC_LBL_V_WIDTH_VAL);
  preview_crosshair_ = CreateWindowExW(
      0, L"STATIC", L"", WS_CHILD | SS_OWNERDRAW, 0, 0, 10, 10, hwnd_,
      reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_PREVIEW_CROSSHAIR)),
      instance_, nullptr);

  // Tab 1: 色彩模式
  sw_auto_theme_ = create_owner_btn(L"", IDC_SW_AUTO_THEME);
  preview_color_a_ = CreateWindowExW(
      0, L"STATIC", L"", WS_CHILD | SS_OWNERDRAW, 0, 0, 10, 10, hwnd_,
      reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_PREVIEW_COLOR_A)),
      instance_, nullptr);
  btn_color_a_ = create_owner_btn(L"选择色彩", IDC_BTN_COLOR_A);
  slider_alpha_a_ = create_slider(IDC_SLIDER_ALPHA_A, 15, 255);
  lbl_alpha_a_val_ = create_val_pill(L"200", IDC_LBL_ALPHA_A_VAL);

  preview_color_b_ = CreateWindowExW(
      0, L"STATIC", L"", WS_CHILD | SS_OWNERDRAW, 0, 0, 10, 10, hwnd_,
      reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_PREVIEW_COLOR_B)),
      instance_, nullptr);
  btn_color_b_ = create_owner_btn(L"选择色彩", IDC_BTN_COLOR_B);
  slider_alpha_b_ = create_slider(IDC_SLIDER_ALPHA_B, 15, 255);
  lbl_alpha_b_val_ = create_val_pill(L"180", IDC_LBL_ALPHA_B_VAL);

  // Tab 2: 快捷键
  key_toggle_ctrl_ = create_owner_btn(L"Ctrl", IDC_KEY_TOGGLE_CTRL);
  key_toggle_alt_ = create_owner_btn(L"Alt", IDC_KEY_TOGGLE_ALT);
  key_toggle_win_ = create_owner_btn(L"Win", IDC_KEY_TOGGLE_WIN);
  key_toggle_shift_ = create_owner_btn(L"Shift", IDC_KEY_TOGGLE_SHIFT);
  combo_toggle_vk_ = create_combo(IDC_COMBO_TOGGLE_VK);

  key_exit_ctrl_ = create_owner_btn(L"Ctrl", IDC_KEY_EXIT_CTRL);
  key_exit_alt_ = create_owner_btn(L"Alt", IDC_KEY_EXIT_ALT);
  key_exit_win_ = create_owner_btn(L"Win", IDC_KEY_EXIT_WIN);
  key_exit_shift_ = create_owner_btn(L"Shift", IDC_KEY_EXIT_SHIFT);
  combo_exit_vk_ = create_combo(IDC_COMBO_EXIT_VK);
  btn_sync_hotkey_ = create_owner_btn(
      L"将退出热键同步为显隐热键（启用单键智能复用）", IDC_BTN_SYNC_HOTKEY);

  // Tab 3: 系统与更新
  sw_show_on_startup_ = create_owner_btn(L"", IDC_SW_SHOW_ON_STARTUP);
  sw_auto_dpi_ = create_owner_btn(L"", IDC_SW_AUTO_DPI);
  sw_autostart_ = create_owner_btn(L"", IDC_SW_AUTOSTART);
  sw_auto_check_update_ = create_owner_btn(L"", IDC_SW_AUTO_UPDATE);
  btn_check_update_ = create_owner_btn(L"立即检查更新", IDC_BTN_CHECK_UPDATE);

  // 底部统一动作栏
  btn_save_ = create_owner_btn(L"保存并应用", IDC_BTN_SAVE, true);
  btn_defaults_ = create_owner_btn(L"恢复默认", IDC_BTN_DEFAULTS, true);
  btn_close_ = create_owner_btn(L"关闭", IDC_BTN_CLOSE, true);
}

void SettingsWindow::LayoutControls(int dpi) {
  if (font_title_ != nullptr) DeleteObject(font_title_);
  if (font_card_title_ != nullptr) DeleteObject(font_card_title_);
  if (font_bold_ != nullptr) DeleteObject(font_bold_);
  if (font_regular_ != nullptr) DeleteObject(font_regular_);
  if (font_small_ != nullptr) DeleteObject(font_small_);

  font_title_ = CreateModernFont(135, FW_SEMIBOLD, dpi);
  font_card_title_ = CreateModernFont(110, FW_SEMIBOLD, dpi);
  font_bold_ = CreateModernFont(95, FW_SEMIBOLD, dpi);
  font_regular_ = CreateModernFont(92, FW_NORMAL, dpi);
  font_small_ = CreateModernFont(83, FW_NORMAL, dpi);

  auto set_font = [&](HWND h, HFONT f) {
    if (h != nullptr) {
      SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
    }
  };
  set_font(combo_toggle_vk_, font_regular_);
  set_font(combo_exit_vk_, font_regular_);

  const int pad_x = Scale(kPadX, dpi);
  const int card_w = Scale(kCardW, dpi);
  const int card_y = Scale(kCardY, dpi);
  const int inner_x = pad_x + Scale(20, dpi);
  const int inner_w = card_w - Scale(40, dpi);

  // Tab 0: 外观尺寸
  SetWindowPos(slider_gap_, nullptr, inner_x + Scale(292, dpi),
               card_y + Scale(84, dpi), Scale(220, dpi), Scale(26, dpi),
               SWP_NOZORDER);
  SetWindowPos(lbl_gap_val_, nullptr, inner_x + Scale(522, dpi),
               card_y + Scale(85, dpi), Scale(70, dpi), Scale(24, dpi),
               SWP_NOZORDER);

  SetWindowPos(slider_h_width_, nullptr, inner_x + Scale(292, dpi),
               card_y + Scale(144, dpi), Scale(220, dpi), Scale(26, dpi),
               SWP_NOZORDER);
  SetWindowPos(lbl_h_width_val_, nullptr, inner_x + Scale(522, dpi),
               card_y + Scale(145, dpi), Scale(70, dpi), Scale(24, dpi),
               SWP_NOZORDER);

  SetWindowPos(slider_v_width_, nullptr, inner_x + Scale(292, dpi),
               card_y + Scale(204, dpi), Scale(220, dpi), Scale(26, dpi),
               SWP_NOZORDER);
  SetWindowPos(lbl_v_width_val_, nullptr, inner_x + Scale(522, dpi),
               card_y + Scale(205, dpi), Scale(70, dpi), Scale(24, dpi),
               SWP_NOZORDER);

  SetWindowPos(preview_crosshair_, nullptr, inner_x, card_y + Scale(282, dpi),
               inner_w, Scale(122, dpi), SWP_NOZORDER);

  // Tab 1: 色彩模式
  const int sw_w = Scale(46, dpi);
  const int sw_h = Scale(26, dpi);
  SetWindowPos(sw_auto_theme_, nullptr, inner_x + inner_w - sw_w,
               card_y + Scale(88, dpi), sw_w, sw_h, SWP_NOZORDER);

  SetWindowPos(preview_color_a_, nullptr, inner_x + Scale(252, dpi),
               card_y + Scale(150, dpi), Scale(38, dpi), Scale(26, dpi),
               SWP_NOZORDER);
  SetWindowPos(btn_color_a_, nullptr, inner_x + Scale(298, dpi),
               card_y + Scale(149, dpi), Scale(88, dpi), Scale(28, dpi),
               SWP_NOZORDER);
  SetWindowPos(slider_alpha_a_, nullptr, inner_x + Scale(394, dpi),
               card_y + Scale(150, dpi), Scale(128, dpi), Scale(26, dpi),
               SWP_NOZORDER);
  SetWindowPos(lbl_alpha_a_val_, nullptr, inner_x + Scale(530, dpi),
               card_y + Scale(151, dpi), Scale(62, dpi), Scale(24, dpi),
               SWP_NOZORDER);

  SetWindowPos(preview_color_b_, nullptr, inner_x + Scale(252, dpi),
               card_y + Scale(214, dpi), Scale(38, dpi), Scale(26, dpi),
               SWP_NOZORDER);
  SetWindowPos(btn_color_b_, nullptr, inner_x + Scale(298, dpi),
               card_y + Scale(213, dpi), Scale(88, dpi), Scale(28, dpi),
               SWP_NOZORDER);
  SetWindowPos(slider_alpha_b_, nullptr, inner_x + Scale(394, dpi),
               card_y + Scale(214, dpi), Scale(128, dpi), Scale(26, dpi),
               SWP_NOZORDER);
  SetWindowPos(lbl_alpha_b_val_, nullptr, inner_x + Scale(530, dpi),
               card_y + Scale(215, dpi), Scale(62, dpi), Scale(24, dpi),
               SWP_NOZORDER);

  // Tab 2: 快捷键
  const int k_x = inner_x + Scale(222, dpi);
  const int key_w = Scale(50, dpi);
  const int key_h = Scale(28, dpi);
  const int key_step = Scale(56, dpi);
  SetWindowPos(key_toggle_ctrl_, nullptr, k_x, card_y + Scale(87, dpi), key_w,
               key_h, SWP_NOZORDER);
  SetWindowPos(key_toggle_alt_, nullptr, k_x + key_step,
               card_y + Scale(87, dpi), key_w, key_h, SWP_NOZORDER);
  SetWindowPos(key_toggle_win_, nullptr, k_x + key_step * 2,
               card_y + Scale(87, dpi), key_w, key_h, SWP_NOZORDER);
  SetWindowPos(key_toggle_shift_, nullptr, k_x + key_step * 3,
               card_y + Scale(87, dpi), key_w, key_h, SWP_NOZORDER);
  SetWindowPos(combo_toggle_vk_, nullptr, k_x + Scale(230, dpi),
               card_y + Scale(88, dpi), Scale(140, dpi), Scale(220, dpi),
               SWP_NOZORDER);

  SetWindowPos(key_exit_ctrl_, nullptr, k_x, card_y + Scale(153, dpi), key_w,
               key_h, SWP_NOZORDER);
  SetWindowPos(key_exit_alt_, nullptr, k_x + key_step,
               card_y + Scale(153, dpi), key_w, key_h, SWP_NOZORDER);
  SetWindowPos(key_exit_win_, nullptr, k_x + key_step * 2,
               card_y + Scale(153, dpi), key_w, key_h, SWP_NOZORDER);
  SetWindowPos(key_exit_shift_, nullptr, k_x + key_step * 3,
               card_y + Scale(153, dpi), key_w, key_h, SWP_NOZORDER);
  SetWindowPos(combo_exit_vk_, nullptr, k_x + Scale(230, dpi),
               card_y + Scale(154, dpi), Scale(140, dpi), Scale(220, dpi),
               SWP_NOZORDER);

  SetWindowPos(btn_sync_hotkey_, nullptr, inner_x, card_y + Scale(210, dpi),
               inner_w, Scale(34, dpi), SWP_NOZORDER);

  // Tab 3: 系统与更新 (右侧对齐 macOS 胶囊开关)
  const int sw_x = inner_x + inner_w - sw_w;
  SetWindowPos(sw_show_on_startup_, nullptr, sw_x, card_y + Scale(85, dpi),
               sw_w, sw_h, SWP_NOZORDER);
  SetWindowPos(sw_auto_dpi_, nullptr, sw_x, card_y + Scale(143, dpi), sw_w,
               sw_h, SWP_NOZORDER);
  SetWindowPos(sw_autostart_, nullptr, sw_x, card_y + Scale(201, dpi), sw_w,
               sw_h, SWP_NOZORDER);
  SetWindowPos(sw_auto_check_update_, nullptr, sw_x, card_y + Scale(259, dpi),
               sw_w, sw_h, SWP_NOZORDER);
  SetWindowPos(btn_check_update_, nullptr, inner_x, card_y + Scale(332, dpi),
               Scale(148, dpi), Scale(36, dpi), SWP_NOZORDER);

  // 底部统一动作栏
  const int btn_y = Scale(kFooterY, dpi);
  const int btn_h = Scale(kFooterH, dpi);
  SetWindowPos(btn_save_, nullptr, pad_x, btn_y, Scale(136, dpi), btn_h,
               SWP_NOZORDER);
  SetWindowPos(btn_defaults_, nullptr, pad_x + Scale(148, dpi), btn_y,
               Scale(108, dpi), btn_h, SWP_NOZORDER);
  SetWindowPos(btn_close_, nullptr, pad_x + card_w - Scale(92, dpi), btn_y,
               Scale(92, dpi), btn_h, SWP_NOZORDER);
}

void SettingsWindow::UpdateTabVisibility() {
  auto show_ctrls = [&](const std::vector<HWND>& ctrls, bool show) {
    for (HWND h : ctrls) {
      if (h != nullptr) ShowWindow(h, show ? SW_SHOW : SW_HIDE);
    }
  };

  show_ctrls({slider_gap_, lbl_gap_val_, slider_h_width_, lbl_h_width_val_,
              slider_v_width_, lbl_v_width_val_, preview_crosshair_},
             current_tab_ == 0);

  show_ctrls({sw_auto_theme_, preview_color_a_, btn_color_a_, slider_alpha_a_,
              lbl_alpha_a_val_, preview_color_b_, btn_color_b_,
              slider_alpha_b_, lbl_alpha_b_val_},
             current_tab_ == 1);

  show_ctrls({key_toggle_ctrl_, key_toggle_alt_, key_toggle_win_,
              key_toggle_shift_, combo_toggle_vk_, key_exit_ctrl_,
              key_exit_alt_, key_exit_win_, key_exit_shift_, combo_exit_vk_,
              btn_sync_hotkey_},
             current_tab_ == 2);

  show_ctrls({sw_show_on_startup_, sw_auto_dpi_, sw_autostart_,
              sw_auto_check_update_, btn_check_update_},
             current_tab_ == 3);
}

void SettingsWindow::SwitchTab(int new_tab) {
  if (new_tab < 0 || new_tab > 3 || new_tab == current_tab_) return;
  current_tab_ = new_tab;
  UpdateTabVisibility();
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsWindow::ShowToast(const std::wstring& text, bool success) {
  toast_text_ = text;
  toast_success_ = success;
  toast_visible_ = true;
  if (hwnd_ != nullptr) {
    SetTimer(hwnd_, kToastTimerId, 2800, nullptr);
    InvalidateRect(hwnd_, nullptr, FALSE);
  }
}

void SettingsWindow::SyncColorTabControls() {
  const LineColor& col_a =
      draft_auto_theme_ ? temp_light_color_ : temp_horiz_color_;
  const LineColor& col_b =
      draft_auto_theme_ ? temp_dark_color_ : temp_vert_color_;

  SendMessageW(slider_alpha_a_, TBM_SETPOS, TRUE, col_a.alpha.value);
  SetWindowTextW(lbl_alpha_a_val_,
                 std::to_wstring(col_a.alpha.value).c_str());

  SendMessageW(slider_alpha_b_, TBM_SETPOS, TRUE, col_b.alpha.value);
  SetWindowTextW(lbl_alpha_b_val_,
                 std::to_wstring(col_b.alpha.value).c_str());

  InvalidateRect(preview_color_a_, nullptr, FALSE);
  InvalidateRect(preview_color_b_, nullptr, FALSE);
  InvalidateRect(lbl_alpha_a_val_, nullptr, FALSE);
  InvalidateRect(lbl_alpha_b_val_, nullptr, FALSE);
  InvalidateRect(preview_crosshair_, nullptr, FALSE);
  InvalidateRect(hwnd_, nullptr, FALSE);
}

unsigned int SettingsWindow::GetSelectedVk(HWND combo,
                                           unsigned int fallback) const {
  const int idx =
      static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
  const auto& keys = GetKeyList();
  if (idx >= 0 && idx < static_cast<int>(keys.size())) {
    return keys[static_cast<size_t>(idx)].second;
  }
  return fallback;
}

std::wstring SettingsWindow::FormatShortcutPreview(unsigned int mod,
                                                   unsigned int vk) const {
  std::wstring s;
  if ((mod & MOD_CONTROL) != 0) s += L"Ctrl+";
  if ((mod & MOD_ALT) != 0) s += L"Alt+";
  if ((mod & MOD_WIN) != 0) s += L"Win+";
  if ((mod & MOD_SHIFT) != 0) s += L"Shift+";
  s += Config::VKToString(vk);
  return s;
}

void SettingsWindow::LoadValuesToUI() {
  // Gap & Width
  SendMessageW(slider_gap_, TBM_SETPOS, TRUE,
               std::min(config_.gap, 120U));
  SetWindowTextW(lbl_gap_val_, (std::to_wstring(config_.gap) + L" px").c_str());

  SendMessageW(slider_h_width_, TBM_SETPOS, TRUE,
               std::min(config_.horizontal.width, 32U));
  SetWindowTextW(lbl_h_width_val_,
                 (std::to_wstring(config_.horizontal.width) + L" px").c_str());

  SendMessageW(slider_v_width_, TBM_SETPOS, TRUE,
               std::min(config_.vertical.width, 32U));
  SetWindowTextW(lbl_v_width_val_,
                 (std::to_wstring(config_.vertical.width) + L" px").c_str());

  // Theme & Colors
  draft_auto_theme_ = config_.auto_theme;
  temp_light_color_ = config_.light_theme;
  temp_dark_color_ = config_.dark_theme;
  temp_horiz_color_ = {config_.horizontal.r, config_.horizontal.g,
                       config_.horizontal.b, config_.horizontal.alpha};
  temp_vert_color_ = {config_.vertical.r, config_.vertical.g,
                      config_.vertical.b, config_.vertical.alpha};
  SyncColorTabControls();

  // Hotkeys
  draft_toggle_mod_ = config_.hotkey_h_s.mod;
  draft_exit_mod_ = config_.hotkey_exit.mod;
  const auto& keys = GetKeyList();
  for (size_t i = 0; i < keys.size(); ++i) {
    if (keys[i].second == config_.hotkey_h_s.vk) {
      SendMessageW(combo_toggle_vk_, CB_SETCURSEL, i, 0);
      break;
    }
  }
  for (size_t i = 0; i < keys.size(); ++i) {
    if (keys[i].second == config_.hotkey_exit.vk) {
      SendMessageW(combo_exit_vk_, CB_SETCURSEL, i, 0);
      break;
    }
  }

  // System
  draft_show_on_startup_ = config_.show_on_startup;
  draft_auto_dpi_ = config_.auto_dpi;
  draft_autostart_ = autostart::IsAutoStartEnabled();
  draft_auto_check_update_ = config_.auto_check_update;

  RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

bool SettingsWindow::SaveValuesFromUI() {
  Config updated = config_;

  // Gap & Width
  updated.gap =
      static_cast<unsigned int>(SendMessageW(slider_gap_, TBM_GETPOS, 0, 0));
  updated.horizontal.width = static_cast<unsigned int>(
      SendMessageW(slider_h_width_, TBM_GETPOS, 0, 0));
  updated.vertical.width = static_cast<unsigned int>(
      SendMessageW(slider_v_width_, TBM_GETPOS, 0, 0));

  // Theme & Colors
  updated.auto_theme = draft_auto_theme_;
  updated.light_theme = temp_light_color_;
  updated.dark_theme = temp_dark_color_;
  updated.horizontal.r = temp_horiz_color_.r;
  updated.horizontal.g = temp_horiz_color_.g;
  updated.horizontal.b = temp_horiz_color_.b;
  updated.horizontal.alpha = temp_horiz_color_.alpha;
  updated.vertical.r = temp_vert_color_.r;
  updated.vertical.g = temp_vert_color_.g;
  updated.vertical.b = temp_vert_color_.b;
  updated.vertical.alpha = temp_vert_color_.alpha;

  // Hotkeys
  updated.hotkey_h_s.mod = draft_toggle_mod_;
  updated.hotkey_h_s.vk = GetSelectedVk(combo_toggle_vk_, 'E');
  updated.hotkey_exit.mod = draft_exit_mod_;
  updated.hotkey_exit.vk = GetSelectedVk(combo_exit_vk_, 'Q');

  // System
  updated.show_on_startup = draft_show_on_startup_;
  updated.auto_dpi = draft_auto_dpi_;
  updated.auto_check_update = draft_auto_check_update_;
  updated.ClampAll();

  // 先验证热键并应用到运行期，避免热键冲突时把损坏配置落盘
  if (on_apply_ && !on_apply_(updated)) {
    ShowToast(L"新热键已被其他程序占用", false);
    liquid_glass::ShowAlert(
        hwnd_, L"热键冲突",
        L"新快捷键已被系统或其他程序占用，请更换其他组合键后重试。",
        liquid_glass::OrbKind::kWarning);
    return false;
  }

  // 原子落盘到 CrossPointer.ini
  if (!updated.Save(config_path_.c_str())) {
    if (on_apply_) (void)on_apply_(config_);
    ShowToast(L"配置文件写入失败", false);
    liquid_glass::ShowAlert(
        hwnd_, L"保存失败",
        L"无法写入 CrossPointer.ini，请检查程序所在目录的写入权限。",
        liquid_glass::OrbKind::kError);
    return false;
  }

  (void)autostart::SetAutoStart(draft_autostart_);
  config_ = updated;

  const bool same_hotkey =
      (updated.hotkey_h_s.mod == updated.hotkey_exit.mod &&
       updated.hotkey_h_s.vk == updated.hotkey_exit.vk);
  ShowToast(same_hotkey ? L"已保存并生效 · 单键复用模式" : L"已保存并即时生效",
            true);
  return true;
}

LRESULT CALLBACK SettingsWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp,
                                         LPARAM lp) noexcept {
  SettingsWindow* self = reinterpret_cast<SettingsWindow*>(
      GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_NCCREATE) {
    const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lp);
    self = static_cast<SettingsWindow*>(create->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    return TRUE;
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

void SettingsWindow::PaintSliderControl(HWND slider, HDC screen_hdc) {
  RECT rc = {};
  GetClientRect(slider, &rc);
  const int w = rc.right - rc.left;
  const int h = rc.bottom - rc.top;
  if (w <= 0 || h <= 0) return;

  const int dpi = GetWindowDpi(hwnd_);
  HDC mem_dc = CreateCompatibleDC(screen_hdc);
  HBITMAP bmp = CreateCompatibleBitmap(screen_hdc, w, h);
  HBITMAP old_bmp = static_cast<HBITMAP>(SelectObject(mem_dc, bmp));

  FillRect(mem_dc, &rc, br_card_);

  {
    Gdiplus::Graphics g(mem_dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

    RECT chan = {};
    SendMessageW(slider, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&chan));
    RECT thumb = {};
    SendMessageW(slider, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));

    const float pad_x = ScaleF(9.0f, dpi);
    const float track_x =
        (chan.right > chan.left) ? static_cast<float>(chan.left) : pad_x;
    const float track_w =
        (chan.right > chan.left)
            ? std::max(8.0f, static_cast<float>(chan.right - chan.left))
            : std::max(8.0f, static_cast<float>(w) - pad_x * 2.0f);
    const float track_h = std::max(4.0f, ScaleF(5.0f, dpi));
    const float cy = static_cast<float>(h) * 0.5f;
    const float track_y = cy - track_h * 0.5f;

    // 背景轨道
    Gdiplus::GraphicsPath track_path;
    BuildRoundedPath(track_path, track_x, track_y, track_w, track_h,
                     track_h * 0.5f);
    Gdiplus::SolidBrush track_br(
        is_dark_mode_ ? Gdiplus::Color(255, 76, 80, 90)
                      : Gdiplus::Color(255, 214, 220, 230));
    g.FillPath(&track_br, &track_path);

    // 进度位置计算
    const int min_v =
        static_cast<int>(SendMessageW(slider, TBM_GETRANGEMIN, 0, 0));
    const int max_v =
        static_cast<int>(SendMessageW(slider, TBM_GETRANGEMAX, 0, 0));
    const int pos = static_cast<int>(SendMessageW(slider, TBM_GETPOS, 0, 0));
    const float ratio =
        (max_v > min_v)
            ? std::clamp(static_cast<float>(pos - min_v) /
                             static_cast<float>(max_v - min_v),
                         0.0f, 1.0f)
            : 0.0f;

    const float knob_cx =
        (thumb.right > thumb.left)
            ? static_cast<float>(thumb.left + thumb.right) * 0.5f
            : (track_x + track_w * ratio);

    // macOS Accent 蓝色已填充进度段
    const float fill_w = std::clamp(knob_cx - track_x, 0.0f, track_w);
    if (fill_w > 1.5f) {
      Gdiplus::GraphicsPath fill_path;
      BuildRoundedPath(fill_path, track_x, track_y, std::max(track_h, fill_w),
                       track_h, track_h * 0.5f);
      Gdiplus::SolidBrush accent_br(ToGdip(cr_accent_));
      g.FillPath(&accent_br, &fill_path);
    }

    // 白色立体圆形滑块拨钮
    const bool hot = (hovered_ctrl_ == slider) || (GetFocus() == slider);
    const float rad = ScaleF(hot ? 8.0f : 7.4f, dpi);

    Gdiplus::SolidBrush shadow_br(
        Gdiplus::Color(is_dark_mode_ ? 95 : 42, 0, 0, 0));
    g.FillEllipse(&shadow_br, knob_cx - rad, cy - rad + ScaleF(1.2f, dpi),
                  rad * 2.0f, rad * 2.0f);

    Gdiplus::LinearGradientBrush knob_br(
        Gdiplus::PointF(knob_cx, cy - rad), Gdiplus::PointF(knob_cx, cy + rad),
        Gdiplus::Color(255, 255, 255, 255),
        Gdiplus::Color(255, 238, 242, 248));
    g.FillEllipse(&knob_br, knob_cx - rad, cy - rad, rad * 2.0f, rad * 2.0f);

    Gdiplus::Pen rim_pen(
        hot ? ToGdip(cr_accent_, 220)
            : Gdiplus::Color(is_dark_mode_ ? 95 : 135, 162, 170, 184),
        ScaleF(1.1f, dpi));
    g.DrawEllipse(&rim_pen, knob_cx - rad, cy - rad, rad * 2.0f, rad * 2.0f);
  }

  BitBlt(screen_hdc, 0, 0, w, h, mem_dc, 0, 0, SRCCOPY);
  SelectObject(mem_dc, old_bmp);
  DeleteObject(bmp);
  DeleteDC(mem_dc);
}

void SettingsWindow::DrawOwnerControl(const DRAWITEMSTRUCT* dis) {
  if (dis == nullptr) return;
  const RECT& rc = dis->rcItem;
  const int w = rc.right - rc.left;
  const int h = rc.bottom - rc.top;
  if (w <= 0 || h <= 0) return;

  const int dpi = GetWindowDpi(hwnd_);
  const UINT id = dis->CtlID;
  const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
  const bool focused = (dis->itemState & ODS_FOCUS) != 0;
  const bool hovered = (dis->hwndItem == hovered_ctrl_);

  // 双缓冲离屏绘制，彻底杜绝闪烁
  HDC mem_dc = CreateCompatibleDC(dis->hDC);
  HBITMAP bmp = CreateCompatibleBitmap(dis->hDC, w, h);
  HBITMAP old_bmp = static_cast<HBITMAP>(SelectObject(mem_dc, bmp));

  const bool is_footer_ctrl =
      (id == IDC_BTN_SAVE || id == IDC_BTN_DEFAULTS || id == IDC_BTN_CLOSE);
  RECT local_rc = {0, 0, w, h};
  FillRect(mem_dc, &local_rc, is_footer_ctrl ? br_bg_ : br_card_);

  {
    Gdiplus::Graphics g(mem_dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

    if (IsToggleSwitchControl(id)) {
      bool checked = false;
      if (id == IDC_SW_AUTO_THEME) checked = draft_auto_theme_;
      else if (id == IDC_SW_SHOW_ON_STARTUP) checked = draft_show_on_startup_;
      else if (id == IDC_SW_AUTO_DPI) checked = draft_auto_dpi_;
      else if (id == IDC_SW_AUTOSTART) checked = draft_autostart_;
      else if (id == IDC_SW_AUTO_UPDATE) checked = draft_auto_check_update_;

      const float pad = ScaleF(1.5f, dpi);
      const float tw = static_cast<float>(w) - pad * 2.0f;
      const float th = static_cast<float>(h) - pad * 2.0f;
      const float tr = th * 0.5f;

      Gdiplus::GraphicsPath track;
      BuildRoundedPath(track, pad, pad, tw, th, tr);

      Gdiplus::Color track_col =
          checked
              ? ToGdip(cr_green_)
              : (is_dark_mode_ ? Gdiplus::Color(255, 76, 80, 90)
                               : Gdiplus::Color(255, 206, 211, 222));
      if (hovered && !pressed) {
        track_col = checked
                        ? Gdiplus::Color(255, 58, 214, 98)
                        : (is_dark_mode_ ? Gdiplus::Color(255, 90, 95, 106)
                                         : Gdiplus::Color(255, 194, 200, 214));
      }
      Gdiplus::SolidBrush track_br(track_col);
      g.FillPath(&track_br, &track);

      // 顶部微高光
      Gdiplus::Pen rim_pen(
          checked ? Gdiplus::Color(55, 255, 255, 255)
                  : Gdiplus::Color(is_dark_mode_ ? 45 : 75, 255, 255, 255),
          ScaleF(1.0f, dpi));
      g.DrawPath(&rim_pen, &track);

      // 白色立体圆形拨钮
      const float knob_margin = ScaleF(2.5f, dpi);
      const float knob_d = th - knob_margin * 2.0f;
      const float knob_x = checked ? (pad + tw - knob_margin - knob_d)
                                   : (pad + knob_margin);
      const float knob_y = pad + knob_margin;

      Gdiplus::SolidBrush knob_shadow(Gdiplus::Color(52, 0, 0, 0));
      g.FillEllipse(&knob_shadow, knob_x, knob_y + ScaleF(1.0f, dpi), knob_d,
                    knob_d);

      Gdiplus::LinearGradientBrush knob_br(
          Gdiplus::PointF(knob_x, knob_y),
          Gdiplus::PointF(knob_x, knob_y + knob_d),
          Gdiplus::Color(255, 255, 255, 255),
          Gdiplus::Color(255, 240, 243, 248));
      g.FillEllipse(&knob_br, knob_x, knob_y, knob_d, knob_d);

      if (focused) {
        Gdiplus::Pen focus_pen(ToGdip(cr_accent_, 180), ScaleF(1.2f, dpi));
        g.DrawPath(&focus_pen, &track);
      }
    } else if (IsKeycapControl(id)) {
      bool active = false;
      if (id == IDC_KEY_TOGGLE_CTRL) active = (draft_toggle_mod_ & MOD_CONTROL) != 0;
      else if (id == IDC_KEY_TOGGLE_ALT) active = (draft_toggle_mod_ & MOD_ALT) != 0;
      else if (id == IDC_KEY_TOGGLE_WIN) active = (draft_toggle_mod_ & MOD_WIN) != 0;
      else if (id == IDC_KEY_TOGGLE_SHIFT) active = (draft_toggle_mod_ & MOD_SHIFT) != 0;
      else if (id == IDC_KEY_EXIT_CTRL) active = (draft_exit_mod_ & MOD_CONTROL) != 0;
      else if (id == IDC_KEY_EXIT_ALT) active = (draft_exit_mod_ & MOD_ALT) != 0;
      else if (id == IDC_KEY_EXIT_WIN) active = (draft_exit_mod_ & MOD_WIN) != 0;
      else if (id == IDC_KEY_EXIT_SHIFT) active = (draft_exit_mod_ & MOD_SHIFT) != 0;

      const float pad = ScaleF(1.0f, dpi);
      const float r = ScaleF(7.5f, dpi);
      Gdiplus::GraphicsPath path;
      BuildRoundedPath(path, pad, pad, static_cast<float>(w) - pad * 2.0f,
                       static_cast<float>(h) - pad * 2.0f, r);

      if (active) {
        const Gdiplus::Color c1 =
            pressed ? Gdiplus::Color(255, 0, 98, 214)
                    : (hovered ? Gdiplus::Color(255, 26, 142, 255)
                               : ToGdip(cr_accent_));
        const Gdiplus::Color c2 =
            pressed ? Gdiplus::Color(255, 0, 86, 192)
                    : Gdiplus::Color(255, 0, 108, 232);
        Gdiplus::LinearGradientBrush br(Gdiplus::PointF(0.0f, 0.0f),
                                        Gdiplus::PointF(0.0f, static_cast<float>(h)),
                                        c1, c2);
        g.FillPath(&br, &path);
        Gdiplus::Pen rim(Gdiplus::Color(80, 255, 255, 255), ScaleF(1.0f, dpi));
        g.DrawPath(&rim, &path);
      } else {
        const Gdiplus::Color bg =
            is_dark_mode_
                ? (hovered ? Gdiplus::Color(255, 66, 70, 80)
                           : Gdiplus::Color(255, 54, 57, 65))
                : (hovered ? Gdiplus::Color(255, 234, 238, 246)
                           : Gdiplus::Color(255, 242, 245, 250));
        Gdiplus::SolidBrush br(bg);
        g.FillPath(&br, &path);
        Gdiplus::Pen rim(
            focused ? ToGdip(cr_accent_)
                    : (is_dark_mode_ ? Gdiplus::Color(255, 78, 82, 92)
                                     : Gdiplus::Color(255, 210, 216, 226)),
            ScaleF(1.0f, dpi));
        g.DrawPath(&rim, &path);
      }

      wchar_t caption[32] = {};
      GetWindowTextW(dis->hwndItem, caption, _countof(caption));
      SetBkMode(mem_dc, TRANSPARENT);
      SelectFont(mem_dc, active ? font_bold_ : font_regular_);
      SetTextColor(mem_dc, active ? RGB(255, 255, 255) : cr_secondary_);
      DrawTextW(mem_dc, caption, -1, &local_rc,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    } else if (IsOwnerButtonControl(id)) {
      const bool primary = (id == IDC_BTN_SAVE);
      const float pad = ScaleF(1.0f, dpi);
      const float bh = static_cast<float>(h) - pad * 2.0f;
      const float bw = static_cast<float>(w) - pad * 2.0f;
      const float rad = bh * 0.5f;  // macOS 26 Full Pill Capsule

      Gdiplus::GraphicsPath pill;
      BuildRoundedPath(pill, pad, pad, bw, bh, rad);

      if (primary) {
        const Gdiplus::Color top =
            pressed ? Gdiplus::Color(255, 0, 95, 210)
                    : (hovered ? Gdiplus::Color(255, 32, 146, 255)
                               : Gdiplus::Color(255, 16, 134, 255));
        const Gdiplus::Color bot =
            pressed ? Gdiplus::Color(255, 0, 82, 185)
                    : Gdiplus::Color(255, 0, 108, 232);
        Gdiplus::LinearGradientBrush br(
            Gdiplus::PointF(0.0f, pad), Gdiplus::PointF(0.0f, pad + bh), top,
            bot);
        g.FillPath(&br, &pill);

        // 顶部液态玻璃镜面高光
        Gdiplus::GraphicsState st = g.Save();
        g.SetClip(&pill);
        Gdiplus::Pen sheen(Gdiplus::Color(90, 255, 255, 255),
                           ScaleF(1.2f, dpi));
        g.DrawLine(&sheen, pad + rad * 0.5f, pad + ScaleF(1.0f, dpi),
                   pad + bw - rad * 0.5f, pad + ScaleF(1.0f, dpi));
        g.Restore(st);

        Gdiplus::Pen rim(Gdiplus::Color(70, 255, 255, 255), ScaleF(1.0f, dpi));
        g.DrawPath(&rim, &pill);
      } else {
        const Gdiplus::Color top =
            is_dark_mode_
                ? (pressed ? Gdiplus::Color(255, 50, 53, 60)
                           : (hovered ? Gdiplus::Color(255, 74, 78, 88)
                                      : Gdiplus::Color(255, 62, 65, 74)))
                : (pressed ? Gdiplus::Color(255, 224, 229, 238)
                           : (hovered ? Gdiplus::Color(255, 255, 255, 255)
                                      : Gdiplus::Color(255, 248, 250, 253)));
        const Gdiplus::Color bot =
            is_dark_mode_
                ? (pressed ? Gdiplus::Color(255, 44, 46, 52)
                           : (hovered ? Gdiplus::Color(255, 62, 66, 75)
                                      : Gdiplus::Color(255, 52, 55, 63)))
                : (pressed ? Gdiplus::Color(255, 216, 222, 232)
                           : (hovered ? Gdiplus::Color(255, 242, 245, 250)
                                      : Gdiplus::Color(255, 236, 240, 247)));
        Gdiplus::LinearGradientBrush br(
            Gdiplus::PointF(0.0f, pad), Gdiplus::PointF(0.0f, pad + bh), top,
            bot);
        g.FillPath(&br, &pill);

        Gdiplus::Pen rim(
            focused ? ToGdip(cr_accent_)
                    : (is_dark_mode_ ? Gdiplus::Color(70, 255, 255, 255)
                                     : Gdiplus::Color(255, 206, 213, 224)),
            ScaleF(1.0f, dpi));
        g.DrawPath(&rim, &pill);
      }

      wchar_t caption[128] = {};
      GetWindowTextW(dis->hwndItem, caption, _countof(caption));
      SetBkMode(mem_dc, TRANSPARENT);
      SelectFont(mem_dc,
                 (primary || id == IDC_BTN_SYNC_HOTKEY) ? font_bold_
                                                        : font_regular_);
      SetTextColor(mem_dc, primary ? RGB(255, 255, 255) : cr_text_);
      DrawTextW(mem_dc, caption, -1, &local_rc,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    } else if (IsValuePillControl(id)) {
      const float pad = ScaleF(1.0f, dpi);
      const float bh = static_cast<float>(h) - pad * 2.0f;
      Gdiplus::GraphicsPath pill;
      BuildRoundedPath(pill, pad, pad, static_cast<float>(w) - pad * 2.0f, bh,
                       bh * 0.5f);
      Gdiplus::SolidBrush fill(
          is_dark_mode_ ? Gdiplus::Color(255, 55, 58, 66)
                        : Gdiplus::Color(255, 236, 240, 247));
      g.FillPath(&fill, &pill);
      Gdiplus::Pen rim(
          is_dark_mode_ ? Gdiplus::Color(40, 255, 255, 255)
                        : Gdiplus::Color(255, 218, 224, 234),
          ScaleF(1.0f, dpi));
      g.DrawPath(&rim, &pill);

      wchar_t caption[32] = {};
      GetWindowTextW(dis->hwndItem, caption, _countof(caption));
      SetBkMode(mem_dc, TRANSPARENT);
      SelectFont(mem_dc, font_bold_);
      SetTextColor(mem_dc, cr_text_);
      DrawTextW(mem_dc, caption, -1, &local_rc,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    } else if (id == IDC_PREVIEW_COLOR_A || id == IDC_PREVIEW_COLOR_B) {
      const LineColor& col =
          (id == IDC_PREVIEW_COLOR_A)
              ? (draft_auto_theme_ ? temp_light_color_ : temp_horiz_color_)
              : (draft_auto_theme_ ? temp_dark_color_ : temp_vert_color_);

      const float pad = ScaleF(1.0f, dpi);
      const float sw = static_cast<float>(w) - pad * 2.0f;
      const float sh = static_cast<float>(h) - pad * 2.0f;
      const float r = ScaleF(7.0f, dpi);

      Gdiplus::GraphicsPath clip_path;
      BuildRoundedPath(clip_path, pad, pad, sw, sh, r);
      Gdiplus::GraphicsState st = g.Save();
      g.SetClip(&clip_path);

      // 棋盘格底纹，直观展示不透明度 (Alpha)
      const int cell = std::max(4, Scale(5, dpi));
      Gdiplus::SolidBrush c1(is_dark_mode_ ? Gdiplus::Color(255, 52, 54, 60)
                                           : Gdiplus::Color(255, 236, 238, 242));
      Gdiplus::SolidBrush c2(is_dark_mode_ ? Gdiplus::Color(255, 70, 73, 82)
                                           : Gdiplus::Color(255, 214, 218, 226));
      for (int y = 0; y < h; y += cell) {
        for (int x = 0; x < w; x += cell) {
          const bool alt = ((x / cell) + (y / cell)) % 2 == 0;
          g.FillRectangle(alt ? &c1 : &c2, x, y, cell, cell);
        }
      }

      // 叠加真实 RGBA 色彩
      Gdiplus::SolidBrush color_br(Gdiplus::Color(
          col.alpha.value, col.r.value, col.g.value, col.b.value));
      g.FillRectangle(&color_br, 0, 0, w, h);
      g.Restore(st);

      Gdiplus::Pen rim(ToGdip(cr_card_border_), ScaleF(1.0f, dpi));
      g.DrawPath(&rim, &clip_path);
    } else if (id == IDC_PREVIEW_CROSSHAIR) {
      const float pad = ScaleF(1.0f, dpi);
      const float cw = static_cast<float>(w) - pad * 2.0f;
      const float ch = static_cast<float>(h) - pad * 2.0f;
      const float r = ScaleF(10.0f, dpi);

      Gdiplus::GraphicsPath canvas;
      BuildRoundedPath(canvas, pad, pad, cw, ch, r);
      Gdiplus::LinearGradientBrush bg_br(
          Gdiplus::PointF(0.0f, 0.0f),
          Gdiplus::PointF(0.0f, static_cast<float>(h)),
          is_dark_mode_ ? Gdiplus::Color(255, 22, 24, 30)
                        : Gdiplus::Color(255, 245, 247, 252),
          is_dark_mode_ ? Gdiplus::Color(255, 17, 19, 24)
                        : Gdiplus::Color(255, 237, 241, 248));
      g.FillPath(&bg_br, &canvas);

      Gdiplus::GraphicsState st = g.Save();
      g.SetClip(&canvas);

      const float cx = static_cast<float>(w) * 0.5f;
      const float cy = static_cast<float>(h) * 0.5f;

      // 极简微弱对齐参考点阵与中轴线
      Gdiplus::Pen grid_pen(
          is_dark_mode_ ? Gdiplus::Color(42, 160, 180, 210)
                        : Gdiplus::Color(48, 120, 136, 165),
          ScaleF(1.0f, dpi));
      grid_pen.SetDashStyle(Gdiplus::DashStyleDot);
      g.DrawLine(&grid_pen, ScaleF(18.0f, dpi), cy,
                 static_cast<float>(w) - ScaleF(18.0f, dpi), cy);
      g.DrawLine(&grid_pen, cx, ScaleF(12.0f, dpi), cx,
                 static_cast<float>(h) - ScaleF(12.0f, dpi));

      // 使用用户当前实时配置的真实色彩与透明度
      const LineColor& h_col =
          draft_auto_theme_
              ? (is_dark_mode_ ? temp_dark_color_ : temp_light_color_)
              : temp_horiz_color_;
      const LineColor& v_col =
          draft_auto_theme_
              ? (is_dark_mode_ ? temp_dark_color_ : temp_light_color_)
              : temp_vert_color_;

      const int gap =
          static_cast<int>(SendMessageW(slider_gap_, TBM_GETPOS, 0, 0));
      const int h_w =
          static_cast<int>(SendMessageW(slider_h_width_, TBM_GETPOS, 0, 0));
      const int v_w =
          static_cast<int>(SendMessageW(slider_v_width_, TBM_GETPOS, 0, 0));

      const float s_gap = ScaleF(static_cast<float>(gap), dpi);
      const float s_hw = std::max(1.0f, ScaleF(static_cast<float>(h_w), dpi));
      const float s_vw = std::max(1.0f, ScaleF(static_cast<float>(v_w), dpi));
      const float margin_x = ScaleF(20.0f, dpi);
      const float margin_y = ScaleF(12.0f, dpi);

      Gdiplus::SolidBrush h_br(Gdiplus::Color(h_col.alpha.value, h_col.r.value,
                                              h_col.g.value, h_col.b.value));
      Gdiplus::SolidBrush v_br(Gdiplus::Color(v_col.alpha.value, v_col.r.value,
                                              v_col.g.value, v_col.b.value));

      const float left_w = (cx - s_gap) - margin_x;
      const float right_x = cx + s_gap;
      const float right_w = (static_cast<float>(w) - margin_x) - right_x;
      if (left_w > 0.5f) {
        g.FillRectangle(&h_br, margin_x, cy - s_hw * 0.5f, left_w, s_hw);
      }
      if (right_w > 0.5f) {
        g.FillRectangle(&h_br, right_x, cy - s_hw * 0.5f, right_w, s_hw);
      }

      const float top_h = (cy - s_gap) - margin_y;
      const float bot_y = cy + s_gap;
      const float bot_h = (static_cast<float>(h) - margin_y) - bot_y;
      if (top_h > 0.5f) {
        g.FillRectangle(&v_br, cx - s_vw * 0.5f, margin_y, s_vw, top_h);
      }
      if (bot_h > 0.5f) {
        g.FillRectangle(&v_br, cx - s_vw * 0.5f, bot_y, s_vw, bot_h);
      }

      // 中心微缩 macOS 指针箭头，直观对比 Gap 留白
      const float ps = ScaleF(1.0f, dpi);
      const Gdiplus::PointF ptr_pts[7] = {
          {cx, cy},
          {cx, cy + 12.0f * ps},
          {cx + 3.2f * ps, cy + 9.2f * ps},
          {cx + 5.6f * ps, cy + 13.5f * ps},
          {cx + 7.4f * ps, cy + 12.6f * ps},
          {cx + 5.0f * ps, cy + 8.3f * ps},
          {cx + 8.8f * ps, cy + 8.3f * ps}};
      Gdiplus::SolidBrush ptr_fill(Gdiplus::Color(240, 22, 24, 28));
      Gdiplus::Pen ptr_rim(Gdiplus::Color(245, 255, 255, 255),
                           std::max(1.0f, 1.15f * ps));
      g.FillPolygon(&ptr_fill, ptr_pts, 7);
      g.DrawPolygon(&ptr_rim, ptr_pts, 7);

      g.Restore(st);

      Gdiplus::Pen border(ToGdip(cr_card_border_), ScaleF(1.0f, dpi));
      g.DrawPath(&border, &canvas);

      // 右下角实时参数摘要
      const std::wstring note = L"H " + std::to_wstring(h_w) + L"px · V " +
                                std::to_wstring(v_w) + L"px · Gap " +
                                std::to_wstring(gap) + L"px";
      SetBkMode(mem_dc, TRANSPARENT);
      SelectFont(mem_dc, font_small_);
      SetTextColor(mem_dc, cr_secondary_);
      RECT note_rc = {Scale(14, dpi), h - Scale(24, dpi), w - Scale(12, dpi),
                      h - Scale(5, dpi)};
      DrawTextW(mem_dc, note.c_str(), -1, &note_rc,
                DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
  }

  BitBlt(dis->hDC, rc.left, rc.top, w, h, mem_dc, 0, 0, SRCCOPY);
  SelectObject(mem_dc, old_bmp);
  DeleteObject(bmp);
  DeleteDC(mem_dc);
}

void SettingsWindow::PaintWindow(HDC screen_hdc) {
  RECT client = {};
  GetClientRect(hwnd_, &client);
  const int cw = client.right - client.left;
  const int ch = client.bottom - client.top;
  if (cw <= 0 || ch <= 0) return;

  const int dpi = GetWindowDpi(hwnd_);

  // 整窗离屏双缓冲，零闪烁渲染
  HDC hdc = CreateCompatibleDC(screen_hdc);
  HBITMAP bmp = CreateCompatibleBitmap(screen_hdc, cw, ch);
  HBITMAP old_bmp = static_cast<HBITMAP>(SelectObject(hdc, bmp));

  FillRect(hdc, &client, br_bg_);

  const int pad_x = Scale(kPadX, dpi);
  const int card_w = Scale(kCardW, dpi);
  const int seg_x = pad_x;
  const int seg_y = Scale(kSegY, dpi);
  const int seg_w = card_w;
  const int seg_h = Scale(kSegH, dpi);
  const int card_x = pad_x;
  const int card_y = Scale(kCardY, dpi);
  const int card_h = Scale(kCardH, dpi);
  const int inner_x = card_x + Scale(20, dpi);
  const int inner_w = card_w - Scale(40, dpi);

  {
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

    // 1. Hero Header Squircle 图标 (抗锯齿液态钛金渐变 + 激光十字标 + 中心镂空)
    const float icon_s = ScaleF(44.0f, dpi);
    const float icon_x = static_cast<float>(pad_x);
    const float icon_y = ScaleF(15.0f, dpi);
    const float icon_r = ScaleF(12.0f, dpi);

    Gdiplus::GraphicsPath icon_path;
    BuildRoundedPath(icon_path, icon_x, icon_y, icon_s, icon_s, icon_r);
    Gdiplus::LinearGradientBrush icon_bg(
        Gdiplus::PointF(icon_x, icon_y),
        Gdiplus::PointF(icon_x, icon_y + icon_s),
        is_dark_mode_ ? Gdiplus::Color(255, 42, 54, 72)
                      : Gdiplus::Color(255, 36, 48, 66),
        is_dark_mode_ ? Gdiplus::Color(255, 22, 28, 38)
                      : Gdiplus::Color(255, 20, 27, 38));
    g.FillPath(&icon_bg, &icon_path);

    Gdiplus::Pen icon_rim(Gdiplus::Color(90, 160, 210, 255), ScaleF(1.0f, dpi));
    g.DrawPath(&icon_rim, &icon_path);

    const float icx = icon_x + icon_s * 0.5f;
    const float icy = icon_y + icon_s * 0.5f;
    Gdiplus::Pen glow_pen(Gdiplus::Color(70, 0, 210, 255), ScaleF(4.4f, dpi));
    glow_pen.SetStartCap(Gdiplus::LineCapRound);
    glow_pen.SetEndCap(Gdiplus::LineCapRound);
    Gdiplus::Pen laser_pen(Gdiplus::Color(255, 0, 224, 255), ScaleF(2.2f, dpi));
    laser_pen.SetStartCap(Gdiplus::LineCapRound);
    laser_pen.SetEndCap(Gdiplus::LineCapRound);

    const float arm_in = ScaleF(5.6f, dpi);
    const float arm_out = ScaleF(14.5f, dpi);
    for (Gdiplus::Pen* p : {&glow_pen, &laser_pen}) {
      g.DrawLine(p, icx - arm_out, icy, icx - arm_in, icy);
      g.DrawLine(p, icx + arm_in, icy, icx + arm_out, icy);
      g.DrawLine(p, icx, icy - arm_out, icx, icy - arm_in);
      g.DrawLine(p, icx, icy + arm_in, icx, icy + arm_out);
    }
    const float ring_r = ScaleF(3.6f, dpi);
    Gdiplus::Pen ring_pen(Gdiplus::Color(235, 220, 248, 255), ScaleF(1.3f, dpi));
    g.DrawEllipse(&ring_pen, icx - ring_r, icy - ring_r, ring_r * 2.0f,
                  ring_r * 2.0f);

    // 右侧运行状态胶囊 Badge
    const float badge_w = ScaleF(122.0f, dpi);
    const float badge_h = ScaleF(26.0f, dpi);
    const float badge_x = static_cast<float>(cw - pad_x) - badge_w;
    const float badge_y = icon_y + ScaleF(9.0f, dpi);
    const Gdiplus::Color dot_col =
        crosshair_visible_ ? ToGdip(cr_green_) : ToGdip(cr_accent_);

    Gdiplus::GraphicsPath badge_path;
    BuildRoundedPath(badge_path, badge_x, badge_y, badge_w, badge_h,
                     badge_h * 0.5f);
    Gdiplus::SolidBrush badge_bg(
        crosshair_visible_
            ? (is_dark_mode_ ? Gdiplus::Color(255, 24, 46, 34)
                             : Gdiplus::Color(255, 232, 248, 238))
            : (is_dark_mode_ ? Gdiplus::Color(255, 26, 40, 58)
                             : Gdiplus::Color(255, 234, 243, 255)));
    g.FillPath(&badge_bg, &badge_path);
    Gdiplus::Pen badge_rim(
        Gdiplus::Color(115, dot_col.GetR(), dot_col.GetG(), dot_col.GetB()),
        ScaleF(1.0f, dpi));
    g.DrawPath(&badge_rim, &badge_path);

    // 发光状态点
    const float dot_x = badge_x + ScaleF(14.0f, dpi);
    const float dot_y = badge_y + badge_h * 0.5f;
    const float halo_r = ScaleF(5.5f, dpi);
    const float dot_r = ScaleF(3.2f, dpi);
    Gdiplus::SolidBrush halo_br(
        Gdiplus::Color(55, dot_col.GetR(), dot_col.GetG(), dot_col.GetB()));
    g.FillEllipse(&halo_br, dot_x - halo_r, dot_y - halo_r, halo_r * 2.0f,
                  halo_r * 2.0f);
    Gdiplus::SolidBrush dot_br(dot_col);
    g.FillEllipse(&dot_br, dot_x - dot_r, dot_y - dot_r, dot_r * 2.0f,
                  dot_r * 2.0f);

    // Header 细分割线
    Gdiplus::Pen header_div(ToGdip(cr_divider_), ScaleF(1.0f, dpi));
    g.DrawLine(&header_div, static_cast<float>(pad_x), ScaleF(72.0f, dpi),
               static_cast<float>(cw - pad_x), ScaleF(72.0f, dpi));

    // 2. macOS 26 悬浮液态玻璃胶囊 Tab 栏
    Gdiplus::GraphicsPath seg_path;
    BuildRoundedPath(seg_path, static_cast<float>(seg_x),
                     static_cast<float>(seg_y), static_cast<float>(seg_w),
                     static_cast<float>(seg_h), ScaleF(11.0f, dpi));
    Gdiplus::SolidBrush seg_bg(
        is_dark_mode_ ? Gdiplus::Color(255, 38, 40, 46)
                      : Gdiplus::Color(255, 226, 230, 238));
    g.FillPath(&seg_bg, &seg_path);
    Gdiplus::Pen seg_rim(
        is_dark_mode_ ? Gdiplus::Color(40, 255, 255, 255)
                      : Gdiplus::Color(255, 210, 216, 226),
        ScaleF(1.0f, dpi));
    g.DrawPath(&seg_rim, &seg_path);

    const float tab_item_w = static_cast<float>(seg_w) / 4.0f;
    const float tab_pad = ScaleF(3.0f, dpi);
    for (int i = 0; i < 4; ++i) {
      const float tx = static_cast<float>(seg_x) + tab_item_w * i + tab_pad;
      const float ty = static_cast<float>(seg_y) + tab_pad;
      const float tw = tab_item_w - tab_pad * 2.0f;
      const float th = static_cast<float>(seg_h) - tab_pad * 2.0f;

      if (i == current_tab_) {
        Gdiplus::GraphicsPath active_pill;
        BuildRoundedPath(active_pill, tx, ty, tw, th, ScaleF(8.5f, dpi));
        Gdiplus::LinearGradientBrush pill_br(
            Gdiplus::PointF(tx, ty), Gdiplus::PointF(tx, ty + th),
            is_dark_mode_ ? Gdiplus::Color(255, 76, 80, 92)
                          : Gdiplus::Color(255, 255, 255, 255),
            is_dark_mode_ ? Gdiplus::Color(255, 60, 64, 74)
                          : Gdiplus::Color(255, 245, 248, 252));
        g.FillPath(&pill_br, &active_pill);
        Gdiplus::Pen active_rim(
            is_dark_mode_ ? Gdiplus::Color(72, 255, 255, 255)
                          : Gdiplus::Color(255, 212, 218, 228),
            ScaleF(1.0f, dpi));
        g.DrawPath(&active_rim, &active_pill);
      } else if (i == hover_tab_) {
        Gdiplus::GraphicsPath hover_pill;
        BuildRoundedPath(hover_pill, tx, ty, tw, th, ScaleF(8.5f, dpi));
        Gdiplus::SolidBrush h_br(
            is_dark_mode_ ? Gdiplus::Color(55, 255, 255, 255)
                          : Gdiplus::Color(115, 255, 255, 255));
        g.FillPath(&h_br, &hover_pill);
      }

      const Gdiplus::Color glyph_col =
          (i == current_tab_) ? ToGdip(cr_accent_) : ToGdip(cr_secondary_);
      DrawTabGlyph(g, i, tx + ScaleF(28.0f, dpi), ty + th * 0.5f,
                   ScaleF(14.5f, dpi), glyph_col, dpi);
    }

    // 3. 主体 Inset Grouped Liquid Glass 卡片
    DrawLiquidGlassCard(g, static_cast<float>(card_x), static_cast<float>(card_y),
                        static_cast<float>(card_w), static_cast<float>(card_h),
                        ScaleF(14.0f, dpi), is_dark_mode_, dpi);

    // 卡片内部副面板与分割线
    Gdiplus::Pen row_div(ToGdip(cr_divider_), ScaleF(1.0f, dpi));
    auto draw_div = [&](int logical_y_offset) {
      const float y = static_cast<float>(card_y + Scale(logical_y_offset, dpi));
      g.DrawLine(&row_div, static_cast<float>(inner_x), y,
                 static_cast<float>(inner_x + inner_w), y);
    };

    draw_div(66);
    if (current_tab_ == 0) {
      draw_div(126);
      draw_div(186);
      draw_div(246);
    } else if (current_tab_ == 1) {
      draw_div(130);
      draw_div(194);
      draw_div(258);

      // Inset 调色建议玻璃子卡
      Gdiplus::GraphicsPath tip_path;
      BuildRoundedPath(tip_path, static_cast<float>(inner_x),
                       static_cast<float>(card_y + Scale(270, dpi)),
                       static_cast<float>(inner_w), ScaleF(134.0f, dpi),
                       ScaleF(10.0f, dpi));
      Gdiplus::SolidBrush tip_bg(
          is_dark_mode_ ? Gdiplus::Color(255, 32, 40, 52)
                        : Gdiplus::Color(255, 239, 245, 254));
      g.FillPath(&tip_bg, &tip_path);
      Gdiplus::Pen tip_rim(
          is_dark_mode_ ? Gdiplus::Color(85, 64, 130, 210)
                        : Gdiplus::Color(255, 206, 222, 245),
          ScaleF(1.0f, dpi));
      g.DrawPath(&tip_rim, &tip_path);
    } else if (current_tab_ == 2) {
      draw_div(132);
      draw_div(198);

      // Inset 快捷键说明玻璃子卡
      Gdiplus::GraphicsPath tip_path;
      BuildRoundedPath(tip_path, static_cast<float>(inner_x),
                       static_cast<float>(card_y + Scale(256, dpi)),
                       static_cast<float>(inner_w), ScaleF(148.0f, dpi),
                       ScaleF(10.0f, dpi));
      Gdiplus::SolidBrush tip_bg(
          is_dark_mode_ ? Gdiplus::Color(255, 32, 40, 52)
                        : Gdiplus::Color(255, 239, 245, 254));
      g.FillPath(&tip_bg, &tip_path);
      Gdiplus::Pen tip_rim(
          is_dark_mode_ ? Gdiplus::Color(85, 64, 130, 210)
                        : Gdiplus::Color(255, 206, 222, 245),
          ScaleF(1.0f, dpi));
      g.DrawPath(&tip_rim, &tip_path);
    } else if (current_tab_ == 3) {
      draw_div(125);
      draw_div(183);
      draw_div(241);
      draw_div(299);
    }

    // 底部非阻塞保存确认 Toast 胶囊
    if (toast_visible_ && !toast_text_.empty()) {
      const float tx = static_cast<float>(pad_x + Scale(272, dpi));
      const float ty = static_cast<float>(Scale(kFooterY + 4, dpi));
      const float tw = ScaleF(256.0f, dpi);
      const float th = ScaleF(30.0f, dpi);
      Gdiplus::GraphicsPath toast_path;
      BuildRoundedPath(toast_path, tx, ty, tw, th, th * 0.5f);
      Gdiplus::SolidBrush t_bg(
          toast_success_
              ? (is_dark_mode_ ? Gdiplus::Color(255, 24, 48, 35)
                               : Gdiplus::Color(255, 230, 248, 236))
              : (is_dark_mode_ ? Gdiplus::Color(255, 58, 30, 32)
                               : Gdiplus::Color(255, 254, 236, 238)));
      g.FillPath(&t_bg, &toast_path);
      Gdiplus::Pen t_rim(
          toast_success_ ? ToGdip(cr_green_, 140)
                         : Gdiplus::Color(160, 255, 69, 58),
          ScaleF(1.0f, dpi));
      g.DrawPath(&t_rim, &toast_path);
    }
  }

  // 文字层统一使用 ClearType GDI DrawTextW 精准渲染
  SetBkMode(hdc, TRANSPARENT);

  // Header 文字
  SelectFont(hdc, font_title_);
  SetTextColor(hdc, cr_text_);
  RECT title_rc = {pad_x + Scale(58, dpi), Scale(15, dpi),
                   cw - Scale(156, dpi), Scale(40, dpi)};
  DrawTextW(hdc, L"CrossPointer 偏好设置", -1, &title_rc,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

  SelectFont(hdc, font_small_);
  SetTextColor(hdc, cr_secondary_);
  RECT sub_rc = {pad_x + Scale(58, dpi), Scale(40, dpi), cw - Scale(156, dpi),
                 Scale(59, dpi)};
  const std::wstring header_sub =
      std::wstring(app_version::kVersionTag) +
      L" · macOS 26 Liquid Glass · 纯绿色便携";
  DrawTextW(hdc, header_sub.c_str(), -1, &sub_rc,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

  // 状态胶囊文字
  SelectFont(hdc, font_small_);
  SetTextColor(hdc, crosshair_visible_ ? cr_green_ : cr_accent_);
  RECT badge_txt_rc = {cw - pad_x - Scale(100, dpi), Scale(24, dpi),
                       cw - pad_x - Scale(8, dpi), Scale(50, dpi)};
  DrawTextW(hdc, crosshair_visible_ ? L"运行中 · 1ms" : L"托盘待机中", -1,
            &badge_txt_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

  // Tab 标签文字
  const wchar_t* tab_titles[4] = {L"外观尺寸", L"色彩模式", L"全局快捷键",
                                  L"系统与更新"};
  const int tab_w = seg_w / 4;
  for (int i = 0; i < 4; ++i) {
    SelectFont(hdc, i == current_tab_ ? font_bold_ : font_regular_);
    SetTextColor(hdc, i == current_tab_ ? cr_text_ : cr_secondary_);
    RECT trc = {seg_x + i * tab_w + Scale(40, dpi), seg_y,
                seg_x + (i + 1) * tab_w - Scale(8, dpi), seg_y + seg_h};
    DrawTextW(hdc, tab_titles[i], -1, &trc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  }

  // 辅助绘制函数
  auto draw_card_header = [&](const wchar_t* title, const wchar_t* sub) {
    SelectFont(hdc, font_card_title_);
    SetTextColor(hdc, cr_text_);
    RECT hrc = {inner_x, card_y + Scale(16, dpi), inner_x + inner_w,
                card_y + Scale(38, dpi)};
    DrawTextW(hdc, title, -1, &hrc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    SelectFont(hdc, font_small_);
    SetTextColor(hdc, cr_secondary_);
    RECT src = {inner_x, card_y + Scale(40, dpi), inner_x + inner_w,
                card_y + Scale(60, dpi)};
    DrawTextW(hdc, sub, -1, &src,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  };

  auto draw_row = [&](const wchar_t* title, const wchar_t* sub, int y_off,
                      int max_w) {
    SelectFont(hdc, font_bold_);
    SetTextColor(hdc, cr_text_);
    RECT r1 = {inner_x, card_y + Scale(y_off, dpi),
               inner_x + Scale(max_w, dpi), card_y + Scale(y_off + 20, dpi)};
    DrawTextW(hdc, title, -1, &r1,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    SelectFont(hdc, font_small_);
    SetTextColor(hdc, cr_secondary_);
    RECT r2 = {inner_x, card_y + Scale(y_off + 21, dpi),
               inner_x + Scale(max_w, dpi), card_y + Scale(y_off + 42, dpi)};
    DrawTextW(hdc, sub, -1, &r2,
              DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
  };

  if (current_tab_ == 0) {
    draw_card_header(L"十字辅助线几何与尺寸",
                     L"针对 120Hz/240Hz 高刷新率与 8K 高分屏调优，拖动滑块实时预览");
    draw_row(L"中心空隙留白 (Gap)",
             L"指针周围留白距离，0 为无间隙相交（推荐 16 px）", 76, 286);
    draw_row(L"水平横线粗细", L"横向贯穿辅助线的像素宽度（推荐 2 px）", 136, 286);
    draw_row(L"垂直竖线粗细", L"纵向贯穿辅助线的像素宽度（推荐 2 px）", 196, 286);

    SelectFont(hdc, font_bold_);
    SetTextColor(hdc, cr_text_);
    RECT p_rc = {inner_x, card_y + Scale(255, dpi), inner_x + inner_w,
                 card_y + Scale(275, dpi)};
    DrawTextW(hdc, L"实时准星仿真预览", -1, &p_rc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  } else if (current_tab_ == 1) {
    draw_card_header(L"主题自适应与色彩透明度",
                     L"跟随系统浅色/深色主题自动切换，或关闭以独立定制横竖线固定色彩");
    draw_row(L"跟随 Windows 系统深浅色自动切换",
             L"实时响应系统主题广播，在浅色与深色专属配色间无缝自适应", 78, 520);

    if (draft_auto_theme_) {
      draw_row(L"浅色模式专属色彩", L"明亮壁纸、白底文档与表格的高对比配色",
               141, 240);
      draw_row(L"深色模式专属色彩", L"暗黑 IDE、终端与夜间环境下的荧光青蓝",
               205, 240);
    } else {
      draw_row(L"水平横线固定色彩", L"关闭自动主题时，水平辅助线使用的固定配色",
               141, 240);
      draw_row(L"垂直竖线固定色彩", L"关闭自动主题时，垂直辅助线使用的固定配色",
               205, 240);
    }

    const int tip_y = card_y + Scale(270, dpi);
    SelectFont(hdc, font_bold_);
    SetTextColor(hdc, cr_accent_);
    RECT th = {inner_x + Scale(16, dpi), tip_y + Scale(12, dpi),
               inner_x + inner_w - Scale(16, dpi), tip_y + Scale(32, dpi)};
    DrawTextW(hdc, L"调色与透明度建议", -1, &th,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    SelectFont(hdc, font_small_);
    SetTextColor(hdc, cr_secondary_);
    RECT tl1 = {inner_x + Scale(16, dpi), tip_y + Scale(36, dpi),
                inner_x + inner_w - Scale(16, dpi), tip_y + Scale(64, dpi)};
    DrawTextW(hdc,
              L"• 浅色模式推荐高对比沉稳蔚蓝 (#0066E6, Alpha 200)，白底清晰锐利不刺眼。",
              -1, &tl1, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);

    RECT tl2 = {inner_x + Scale(16, dpi), tip_y + Scale(66, dpi),
                inner_x + inner_w - Scale(16, dpi), tip_y + Scale(94, dpi)};
    DrawTextW(hdc,
              L"• 深色模式推荐通透激光青 (#00D2FF, Alpha 180)，夜间代码与设计对齐极佳。",
              -1, &tl2, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);

    RECT tl3 = {inner_x + Scale(16, dpi), tip_y + Scale(96, dpi),
                inner_x + inner_w - Scale(16, dpi), tip_y + Scale(124, dpi)};
    DrawTextW(hdc,
              L"• 右侧滑块支持 15 ~ 255 精细透明度调节，左侧棋盘色块即时预览真实透明效果。",
              -1, &tl3, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
  } else if (current_tab_ == 2) {
    draw_card_header(L"全局快捷键与单键智能复用",
                     L"修饰键按下顺序完全无关；将显隐与退出设为相同按键即可启用单键复用");
    draw_row(L"显隐十字线热键", L"随时呼出或隐藏屏幕十字线", 79, 214);
    draw_row(L"退出程序热键", L"一键退出，可与显隐设为相同", 145, 214);

    const unsigned int t_vk = GetSelectedVk(combo_toggle_vk_, 'E');
    const unsigned int e_vk = GetSelectedVk(combo_exit_vk_, 'Q');
    const bool same = (draft_toggle_mod_ == draft_exit_mod_ && t_vk == e_vk);
    const std::wstring summary =
        same ? (L"当前模式：单键智能复用 (" +
                FormatShortcutPreview(draft_toggle_mod_, t_vk) + L")")
             : (L"当前模式：双键独立  ·  显隐 " +
                FormatShortcutPreview(draft_toggle_mod_, t_vk) + L"  /  退出 " +
                FormatShortcutPreview(draft_exit_mod_, e_vk));

    const int tip_y = card_y + Scale(256, dpi);
    SelectFont(hdc, font_bold_);
    SetTextColor(hdc, same ? cr_green_ : cr_accent_);
    RECT th = {inner_x + Scale(16, dpi), tip_y + Scale(12, dpi),
               inner_x + inner_w - Scale(16, dpi), tip_y + Scale(32, dpi)};
    DrawTextW(hdc, summary.c_str(), -1, &th,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    SelectFont(hdc, font_bold_);
    SetTextColor(hdc, cr_text_);
    RECT f1t = {inner_x + Scale(16, dpi), tip_y + Scale(38, dpi),
                inner_x + inner_w - Scale(16, dpi), tip_y + Scale(56, dpi)};
    DrawTextW(hdc, L"1. 单键智能复用 (Show/Hide + Exit Toggle)", -1, &f1t,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    SelectFont(hdc, font_small_);
    SetTextColor(hdc, cr_secondary_);
    RECT f1b = {inner_x + Scale(28, dpi), tip_y + Scale(57, dpi),
                inner_x + inner_w - Scale(16, dpi), tip_y + Scale(90, dpi)};
    DrawTextW(hdc,
              L"当显隐与退出设为同一组合键时：隐藏状态下按下即刻显示，显示状态下按下即刻退出。",
              -1, &f1b, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);

    SelectFont(hdc, font_bold_);
    SetTextColor(hdc, cr_text_);
    RECT f2t = {inner_x + Scale(16, dpi), tip_y + Scale(92, dpi),
                inner_x + inner_w - Scale(16, dpi), tip_y + Scale(110, dpi)};
    DrawTextW(hdc, L"2. 按键顺序无关识别 (Order-Independent Modifiers)", -1,
              &f2t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    SelectFont(hdc, font_small_);
    SetTextColor(hdc, cr_secondary_);
    RECT f2b = {inner_x + Scale(28, dpi), tip_y + Scale(111, dpi),
                inner_x + inner_w - Scale(16, dpi), tip_y + Scale(142, dpi)};
    DrawTextW(hdc,
              L"Ctrl / Alt / Win / Shift 任意顺序按下配合主键均可精准触发，F1-F24 支持免修饰单键。",
              -1, &f2b, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
  } else if (current_tab_ == 3) {
    draw_card_header(L"系统整合、启动偏好与在线更新",
                     L"100% 纯绿色单文件便携运行，零外部目录外溢，零待机唤醒功耗");
    draw_row(L"启动软件时立即显示十字辅助线",
             L"关闭时静默常驻托盘且保持 0 定时唤醒，随时按快捷键毫秒呼出", 76,
             510);
    draw_row(L"4K / 8K 高 DPI 动态自适应缩放",
             L"PerMonitorV2 顶级感知，跨屏自动按物理 DPI 无损缩放线宽与留白",
             134, 510);
    draw_row(L"开机自动启动 CrossPointer",
             L"仅写入当前用户 HKCU 启动项，免管理员权限，关闭即彻底清除", 192,
             510);
    draw_row(L"启动时自动静默检查新版本",
             L"后台轻量查询 GitHub Releases，支持应用内一键原地安全热替换", 250,
             510);

    SelectFont(hdc, font_bold_);
    SetTextColor(hdc, cr_green_);
    const std::wstring ver_line =
        L"当前版本：CrossPointer " + std::wstring(app_version::kVersionTag) +
        L" · macOS 26 Liquid Glass";
    RECT vrc = {inner_x + Scale(166, dpi), card_y + Scale(318, dpi),
                inner_x + inner_w, card_y + Scale(338, dpi)};
    DrawTextW(hdc, ver_line.c_str(), -1, &vrc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    SelectFont(hdc, font_small_);
    SetTextColor(hdc, cr_secondary_);
    RECT urc = {inner_x + Scale(166, dpi), card_y + Scale(340, dpi),
                inner_x + inner_w, card_y + Scale(358, dpi)};
    DrawTextW(hdc, L"开源仓库：https://github.com/Crucialmer/CrossPointer", -1,
              &urc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    RECT grc = {inner_x + Scale(166, dpi), card_y + Scale(360, dpi),
                inner_x + inner_w, card_y + Scale(378, dpi)};
    DrawTextW(hdc, L"原生 C++20 · Direct2D 硬件加速 · 支持 8K 与 240Hz 高刷屏",
              -1, &grc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  }

  // Toast 文字
  if (toast_visible_ && !toast_text_.empty()) {
    SelectFont(hdc, font_small_);
    SetTextColor(hdc, toast_success_ ? cr_green_ : RGB(255, 69, 58));
    RECT trc = {pad_x + Scale(272, dpi), Scale(kFooterY + 4, dpi),
                pad_x + Scale(272 + 256, dpi), Scale(kFooterY + 34, dpi)};
    const std::wstring prefix = toast_success_ ? L"✓ " : L"! ";
    const std::wstring display = prefix + toast_text_;
    DrawTextW(hdc, display.c_str(), -1, &trc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  }

  BitBlt(screen_hdc, 0, 0, cw, ch, hdc, 0, 0, SRCCOPY);
  SelectObject(hdc, old_bmp);
  DeleteObject(bmp);
  DeleteDC(hdc);
}

LRESULT SettingsWindow::HandleMessage(HWND hwnd, UINT msg, WPARAM wp,
                                      LPARAM lp) {
  switch (msg) {
    case WM_ERASEBKGND:
      return 1;

    case WM_SETTINGCHANGE: {
      UpdateThemeStyles();
      RedrawWindow(hwnd, nullptr, nullptr,
                   RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
      break;
    }

    case WM_DPICHANGED: {
      const int dpi = LOWORD(wp);
      const auto* prc = reinterpret_cast<const RECT*>(lp);
      SetWindowPos(hwnd, nullptr, prc->left, prc->top, prc->right - prc->left,
                   prc->bottom - prc->top, SWP_NOZORDER | SWP_NOACTIVATE);
      LayoutControls(dpi);
      RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
      return 0;
    }

    case WM_TIMER: {
      if (wp == kToastTimerId) {
        KillTimer(hwnd, kToastTimerId);
        toast_visible_ = false;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      break;
    }

    case WM_KEYDOWN: {
      if (wp == VK_ESCAPE) {
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
      const int dpi = GetWindowDpi(hwnd);
      const int mx = GET_X_LPARAM(lp);
      const int my = GET_Y_LPARAM(lp);
      const int seg_x = Scale(kPadX, dpi);
      const int seg_y = Scale(kSegY, dpi);
      const int seg_w = Scale(kCardW, dpi);
      const int seg_h = Scale(kSegH, dpi);
      int new_hover = -1;
      if (mx >= seg_x && mx < seg_x + seg_w && my >= seg_y &&
          my < seg_y + seg_h) {
        new_hover = std::clamp((mx - seg_x) / (seg_w / 4), 0, 3);
      }
      if (new_hover != hover_tab_) {
        hover_tab_ = new_hover;
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      break;
    }

    case WM_MOUSELEAVE: {
      tracking_mouse_ = false;
      if (hover_tab_ != -1) {
        hover_tab_ = -1;
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      break;
    }

    case WM_SETCURSOR: {
      if (reinterpret_cast<HWND>(wp) == hwnd && hover_tab_ != -1) {
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
      }
      break;
    }

    case WM_LBUTTONDOWN: {
      const int dpi = GetWindowDpi(hwnd);
      const int mx = GET_X_LPARAM(lp);
      const int my = GET_Y_LPARAM(lp);
      const int seg_x = Scale(kPadX, dpi);
      const int seg_y = Scale(kSegY, dpi);
      const int seg_w = Scale(kCardW, dpi);
      const int seg_h = Scale(kSegH, dpi);
      if (mx >= seg_x && mx < seg_x + seg_w && my >= seg_y &&
          my < seg_y + seg_h) {
        const int clicked_tab = std::clamp((mx - seg_x) / (seg_w / 4), 0, 3);
        SwitchTab(clicked_tab);
        return 0;
      }
      break;
    }

    case WM_HSCROLL: {
      const HWND sl = reinterpret_cast<HWND>(lp);
      if (sl == slider_gap_) {
        const int val = static_cast<int>(SendMessageW(sl, TBM_GETPOS, 0, 0));
        SetWindowTextW(lbl_gap_val_, (std::to_wstring(val) + L" px").c_str());
        InvalidateRect(lbl_gap_val_, nullptr, FALSE);
        InvalidateRect(preview_crosshair_, nullptr, FALSE);
      } else if (sl == slider_h_width_) {
        const int val = static_cast<int>(SendMessageW(sl, TBM_GETPOS, 0, 0));
        SetWindowTextW(lbl_h_width_val_,
                       (std::to_wstring(val) + L" px").c_str());
        InvalidateRect(lbl_h_width_val_, nullptr, FALSE);
        InvalidateRect(preview_crosshair_, nullptr, FALSE);
      } else if (sl == slider_v_width_) {
        const int val = static_cast<int>(SendMessageW(sl, TBM_GETPOS, 0, 0));
        SetWindowTextW(lbl_v_width_val_,
                       (std::to_wstring(val) + L" px").c_str());
        InvalidateRect(lbl_v_width_val_, nullptr, FALSE);
        InvalidateRect(preview_crosshair_, nullptr, FALSE);
      } else if (sl == slider_alpha_a_) {
        const int val = static_cast<int>(SendMessageW(sl, TBM_GETPOS, 0, 0));
        if (draft_auto_theme_) {
          temp_light_color_.alpha = ColorChannel(val);
        } else {
          temp_horiz_color_.alpha = ColorChannel(val);
        }
        SetWindowTextW(lbl_alpha_a_val_, std::to_wstring(val).c_str());
        InvalidateRect(lbl_alpha_a_val_, nullptr, FALSE);
        InvalidateRect(preview_color_a_, nullptr, FALSE);
        InvalidateRect(preview_crosshair_, nullptr, FALSE);
      } else if (sl == slider_alpha_b_) {
        const int val = static_cast<int>(SendMessageW(sl, TBM_GETPOS, 0, 0));
        if (draft_auto_theme_) {
          temp_dark_color_.alpha = ColorChannel(val);
        } else {
          temp_vert_color_.alpha = ColorChannel(val);
        }
        SetWindowTextW(lbl_alpha_b_val_, std::to_wstring(val).c_str());
        InvalidateRect(lbl_alpha_b_val_, nullptr, FALSE);
        InvalidateRect(preview_color_b_, nullptr, FALSE);
        InvalidateRect(preview_crosshair_, nullptr, FALSE);
      }
      InvalidateRect(sl, nullptr, FALSE);
      return 0;
    }

    case WM_COMMAND: {
      const UINT id = LOWORD(wp);
      const UINT code = HIWORD(wp);

      // ComboBox selection changed -> refresh live hotkey summary
      if (code == CBN_SELCHANGE &&
          (id == IDC_COMBO_TOGGLE_VK || id == IDC_COMBO_EXIT_VK)) {
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }

      if (id == IDCANCEL) {
        Close();
        return 0;
      }

      if (code != BN_CLICKED && code != BN_DOUBLECLICKED) break;

      // Toggle switches
      if (id == IDC_SW_AUTO_THEME) {
        draft_auto_theme_ = !draft_auto_theme_;
        InvalidateRect(sw_auto_theme_, nullptr, FALSE);
        SyncColorTabControls();
        return 0;
      }
      if (id == IDC_SW_SHOW_ON_STARTUP) {
        draft_show_on_startup_ = !draft_show_on_startup_;
        InvalidateRect(sw_show_on_startup_, nullptr, FALSE);
        return 0;
      }
      if (id == IDC_SW_AUTO_DPI) {
        draft_auto_dpi_ = !draft_auto_dpi_;
        InvalidateRect(sw_auto_dpi_, nullptr, FALSE);
        return 0;
      }
      if (id == IDC_SW_AUTOSTART) {
        draft_autostart_ = !draft_autostart_;
        InvalidateRect(sw_autostart_, nullptr, FALSE);
        return 0;
      }
      if (id == IDC_SW_AUTO_UPDATE) {
        draft_auto_check_update_ = !draft_auto_check_update_;
        InvalidateRect(sw_auto_check_update_, nullptr, FALSE);
        return 0;
      }

      // Modifier keycaps
      auto toggle_mod = [&](unsigned int& target, unsigned int flag,
                            HWND ctrl) {
        target ^= flag;
        InvalidateRect(ctrl, nullptr, FALSE);
        InvalidateRect(hwnd, nullptr, FALSE);
      };
      if (id == IDC_KEY_TOGGLE_CTRL) { toggle_mod(draft_toggle_mod_, MOD_CONTROL, key_toggle_ctrl_); return 0; }
      if (id == IDC_KEY_TOGGLE_ALT) { toggle_mod(draft_toggle_mod_, MOD_ALT, key_toggle_alt_); return 0; }
      if (id == IDC_KEY_TOGGLE_WIN) { toggle_mod(draft_toggle_mod_, MOD_WIN, key_toggle_win_); return 0; }
      if (id == IDC_KEY_TOGGLE_SHIFT) { toggle_mod(draft_toggle_mod_, MOD_SHIFT, key_toggle_shift_); return 0; }

      if (id == IDC_KEY_EXIT_CTRL) { toggle_mod(draft_exit_mod_, MOD_CONTROL, key_exit_ctrl_); return 0; }
      if (id == IDC_KEY_EXIT_ALT) { toggle_mod(draft_exit_mod_, MOD_ALT, key_exit_alt_); return 0; }
      if (id == IDC_KEY_EXIT_WIN) { toggle_mod(draft_exit_mod_, MOD_WIN, key_exit_win_); return 0; }
      if (id == IDC_KEY_EXIT_SHIFT) { toggle_mod(draft_exit_mod_, MOD_SHIFT, key_exit_shift_); return 0; }

      if (id == IDC_BTN_SYNC_HOTKEY) {
        draft_exit_mod_ = draft_toggle_mod_;
        const LRESULT cur = SendMessageW(combo_toggle_vk_, CB_GETCURSEL, 0, 0);
        SendMessageW(combo_exit_vk_, CB_SETCURSEL, cur, 0);
        for (HWND h :
             {key_exit_ctrl_, key_exit_alt_, key_exit_win_, key_exit_shift_}) {
          InvalidateRect(h, nullptr, FALSE);
        }
        ShowToast(L"已同步为单键复用，点击保存生效", true);
        return 0;
      }

      if (id == IDC_BTN_CHECK_UPDATE) {
        if (parent_ != nullptr && IsWindow(parent_) != FALSE) {
          PostMessageW(parent_, WM_COMMAND, IDM_TRAY_CHECK_UPDATE, 0);
        }
        return 0;
      }

      if (id == IDC_BTN_SAVE) {
        SaveValuesFromUI();
        return 0;
      }

      if (id == IDC_BTN_CLOSE) {
        Close();
        return 0;
      }

      if (id == IDC_BTN_DEFAULTS) {
        config_ = Config();
        LoadValuesToUI();
        ShowToast(L"已恢复默认参数，点击保存生效", true);
        return 0;
      }

      if (id == IDC_BTN_COLOR_A || id == IDC_BTN_COLOR_B) {
        LineColor& target_col =
            (id == IDC_BTN_COLOR_A)
                ? (draft_auto_theme_ ? temp_light_color_ : temp_horiz_color_)
                : (draft_auto_theme_ ? temp_dark_color_ : temp_vert_color_);
        static COLORREF custom_colors[16] = {0};
        CHOOSECOLORW cc = {sizeof(cc)};
        cc.hwndOwner = hwnd;
        cc.lpCustColors = custom_colors;
        cc.rgbResult = RGB(target_col.r.value, target_col.g.value,
                           target_col.b.value);
        cc.Flags = CC_FULLOPEN | CC_RGBINIT;
        if (ChooseColorW(&cc) != FALSE) {
          target_col.r = ColorChannel(GetRValue(cc.rgbResult));
          target_col.g = ColorChannel(GetGValue(cc.rgbResult));
          target_col.b = ColorChannel(GetBValue(cc.rgbResult));
          InvalidateRect(id == IDC_BTN_COLOR_A ? preview_color_a_
                                               : preview_color_b_,
                         nullptr, FALSE);
          InvalidateRect(preview_crosshair_, nullptr, FALSE);
        }
        return 0;
      }
      break;
    }

    case WM_DRAWITEM: {
      DrawOwnerControl(reinterpret_cast<const DRAWITEMSTRUCT*>(lp));
      return TRUE;
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
      HDC hdc = reinterpret_cast<HDC>(wp);
      SetBkMode(hdc, TRANSPARENT);
      SetTextColor(hdc, cr_text_);
      return reinterpret_cast<LRESULT>(br_card_);
    }

    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLOREDIT: {
      HDC hdc = reinterpret_cast<HDC>(wp);
      SetTextColor(hdc, cr_text_);
      SetBkColor(hdc, is_dark_mode_ ? RGB(52, 54, 60) : RGB(255, 255, 255));
      return reinterpret_cast<LRESULT>(br_combo_);
    }

    case WM_PAINT: {
      PAINTSTRUCT ps = {};
      HDC hdc = BeginPaint(hwnd, &ps);
      PaintWindow(hdc);
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
