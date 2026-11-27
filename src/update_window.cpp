#include "./update_window.h"

#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <thread>

#include "./config.h"
#include "./version.h"

namespace {
constexpr wchar_t kUpdateClassName[] = L"CrossPointerUpdateClass";
constexpr wchar_t kDefaultReleaseUrl[] =
    L"https://github.com/Crucialmer/CrossPointer/releases/latest";

bool HitRect(const RECT& r, int x, int y) noexcept {
  return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

std::wstring FormatNowTime() {
  SYSTEMTIME st = {};
  GetLocalTime(&st);
  wchar_t buf[32] = {};
  swprintf_s(buf, L"今天 %02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
  return buf;
}

std::wstring FormatBytes(std::uint64_t bytes) {
  wchar_t buf[64] = {};
  if (bytes >= 1024ULL * 1024ULL) {
    swprintf_s(buf, L"%.2f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else if (bytes >= 1024ULL) {
    swprintf_s(buf, L"%.1f KB", static_cast<double>(bytes) / 1024.0);
  } else {
    swprintf_s(buf, L"%llu B", static_cast<unsigned long long>(bytes));
  }
  return buf;
}

std::wstring FormatSpeed(double bps) {
  if (bps < 1.0) return L"";
  wchar_t buf[64] = {};
  if (bps >= 1024.0 * 1024.0) {
    swprintf_s(buf, L"%.2f MB/s", bps / (1024.0 * 1024.0));
  } else if (bps >= 1024.0) {
    swprintf_s(buf, L"%.1f KB/s", bps / 1024.0);
  } else {
    swprintf_s(buf, L"%.0f B/s", bps);
  }
  return buf;
}

std::wstring CleanChangelogPreview(const std::wstring& raw) {
  std::wstring out;
  out.reserve(std::min<size_t>(raw.size(), 260));
  for (wchar_t ch : raw) {
    if (ch == L'\r') continue;
    if (ch == L'#' || ch == L'*') continue;
    out.push_back(ch);
    if (out.size() >= 190) {
      out += L"...";
      break;
    }
  }
  while (!out.empty() && (out.front() == L'\n' || out.front() == L' ')) {
    out.erase(out.begin());
  }
  return out;
}

struct DownloadDonePayload {
  bool ok = false;
  std::wstring error_msg;
};
}  // namespace

SoftwareUpdateWindow::~SoftwareUpdateWindow() { Close(); }

void SoftwareUpdateWindow::Close() noexcept {
  if (cancel_flag_) {
    cancel_flag_->store(true);
  }
  if (hwnd_ != nullptr) {
    if (anim_timer_running_) {
      KillTimer(hwnd_, kAnimTimerId);
      anim_timer_running_ = false;
    }
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
  if (font_caption_) { DeleteObject(font_caption_); font_caption_ = nullptr; }
  if (font_app_) { DeleteObject(font_app_); font_app_ = nullptr; }
  if (font_badge_) { DeleteObject(font_badge_); font_badge_ = nullptr; }
  if (font_status_) { DeleteObject(font_status_); font_status_ = nullptr; }
  if (font_body_) { DeleteObject(font_body_); font_body_ = nullptr; }
  if (font_small_) { DeleteObject(font_small_); font_small_ = nullptr; }
  if (font_btn_) { DeleteObject(font_btn_); font_btn_ = nullptr; }
  gdip_.reset();
}

void SoftwareUpdateWindow::RebuildFonts(int dpi) {
  using namespace liquid_glass;
  if (font_caption_) DeleteObject(font_caption_);
  if (font_app_) DeleteObject(font_app_);
  if (font_badge_) DeleteObject(font_badge_);
  if (font_status_) DeleteObject(font_status_);
  if (font_body_) DeleteObject(font_body_);
  if (font_small_) DeleteObject(font_small_);
  if (font_btn_) DeleteObject(font_btn_);

  font_caption_ = CreateModernFont(86, FW_NORMAL, dpi);
  font_app_ = CreateModernFont(150, FW_BOLD, dpi);
  font_badge_ = CreateModernFont(85, FW_SEMIBOLD, dpi);
  font_status_ = CreateModernFont(112, FW_SEMIBOLD, dpi);
  font_body_ = CreateModernFont(90, FW_NORMAL, dpi);
  font_small_ = CreateModernFont(82, FW_NORMAL, dpi);
  font_btn_ = CreateModernFont(93, FW_SEMIBOLD, dpi);
}

bool SoftwareUpdateWindow::EnsureWindowCreated(HINSTANCE instance, HWND controller) {
  instance_ = instance;
  controller_ = controller;
  if (hwnd_ != nullptr) {
    ShowWindow(hwnd_, SW_RESTORE);
    SetForegroundWindow(hwnd_);
    return true;
  }

  pal_ = liquid_glass::GetPalette(!IsSystemInLightTheme());
  gdip_ = std::make_unique<liquid_glass::GdiplusSession>();
  hover_close_ = press_close_ = false;
  hover_btn_ = press_btn_ = -1;
  tracking_mouse_ = false;

  WNDCLASSEXW wc = {sizeof(wc)};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = instance_;
  wc.lpszClassName = kUpdateClassName;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(101));
  RegisterClassExW(&wc);

  POINT pt = {};
  GetCursorPos(&pt);
  const int dpi = liquid_glass::GetMonitorDpiForPoint(pt);
  const RECT work = liquid_glass::GetMonitorWorkAreaForPoint(pt);
  RebuildFonts(dpi);

  const int win_w = liquid_glass::Scale(412, dpi);
  const int win_h = liquid_glass::Scale(412, dpi);
  const int x = work.left + ((work.right - work.left) - win_w) / 2;
  const int y = work.top + ((work.bottom - work.top) - win_h) / 2;

  hwnd_ = CreateWindowExW(WS_EX_APPWINDOW, kUpdateClassName,
                          L"软件更新 - CrossPointer", WS_POPUP, x, y, win_w,
                          win_h, controller_, nullptr, instance_, this);
  if (hwnd_ == nullptr) {
    Close();
    return false;
  }

  liquid_glass::ApplyWindowChrome(hwnd_, pal_, true);
  ShowWindow(hwnd_, SW_SHOW);
  UpdateWindow(hwnd_);
  SetForegroundWindow(hwnd_);
  return true;
}

void SoftwareUpdateWindow::ShowAndCheck(HINSTANCE instance, HWND controller) {
  const bool was_open = (hwnd_ != nullptr);
  if (!EnsureWindowCreated(instance, controller)) return;
  if (was_open && (state_ == State::kChecking || state_ == State::kDownloading ||
                   state_ == State::kInstalling)) {
    return;
  }
  BeginCheck();
}

void SoftwareUpdateWindow::ShowWithResult(HINSTANCE instance, HWND controller,
                                  const updater::UpdateResult& result) {
  if (!EnsureWindowCreated(instance, controller)) return;
  result_ = result;
  last_check_time_ = FormatNowTime();
  if (result_.has_update) {
    state_ = State::kAvailable;
  } else if (result_.success) {
    state_ = State::kUpToDate;
  } else {
    state_ = State::kError;
    error_text_ = result_.error_msg;
  }
  UpdateAnimationTimer();
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void SoftwareUpdateWindow::StartSilentCheck(HWND controller, UINT notify_msg) {
  std::thread([controller, notify_msg]() {
    const updater::UpdateResult res = updater::CheckUpdateSync();
    if (res.has_update && controller != nullptr &&
        IsWindow(controller) != FALSE) {
      auto* payload = new updater::UpdateResult(res);
      if (PostMessageW(controller, notify_msg, 0,
                       reinterpret_cast<LPARAM>(payload)) == FALSE) {
        delete payload;
      }
    }
  }).detach();
}

void SoftwareUpdateWindow::NotifyThemeChanged() noexcept {
  if (hwnd_ == nullptr) return;
  pal_ = liquid_glass::GetPalette(!IsSystemInLightTheme());
  liquid_glass::ApplyWindowChrome(hwnd_, pal_, true);
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void SoftwareUpdateWindow::UpdateAnimationTimer() {
  if (hwnd_ == nullptr) return;
  const bool need_anim =
      (state_ == State::kChecking) ||
      (state_ == State::kDownloading && progress_.indeterminate);
  if (need_anim && !anim_timer_running_) {
    SetTimer(hwnd_, kAnimTimerId, 30, nullptr);
    anim_timer_running_ = true;
  } else if (!need_anim && anim_timer_running_) {
    KillTimer(hwnd_, kAnimTimerId);
    anim_timer_running_ = false;
  }
}

void SoftwareUpdateWindow::BeginCheck() {
  state_ = State::kChecking;
  error_text_.clear();
  anim_phase_ = 0.0f;
  UpdateAnimationTimer();
  InvalidateRect(hwnd_, nullptr, FALSE);

  const HWND target_hwnd = hwnd_;
  std::thread([target_hwnd]() {
    const updater::UpdateResult res = updater::CheckUpdateSync();
    if (target_hwnd != nullptr && IsWindow(target_hwnd) != FALSE) {
      auto* payload = new updater::UpdateResult(res);
      if (PostMessageW(target_hwnd, kMsgCheckDone, 0,
                       reinterpret_cast<LPARAM>(payload)) == FALSE) {
        delete payload;
      }
    }
  }).detach();
}

void SoftwareUpdateWindow::BeginDownload() {
  state_ = State::kDownloading;
  progress_ = {};
  progress_.indeterminate = true;
  progress_.stage_text = L"正在连接高速下载节点...";
  error_text_.clear();
  cancel_flag_ = std::make_shared<std::atomic<bool>>(false);
  UpdateAnimationTimer();
  InvalidateRect(hwnd_, nullptr, FALSE);

  const HWND target_hwnd = hwnd_;
  const HWND ctrl_hwnd = controller_;
  const updater::UpdateResult info = result_;
  const auto cancel = cancel_flag_;

  std::thread([target_hwnd, ctrl_hwnd, info, cancel]() {
    std::wstring err;
    const bool ok = updater::PerformDirectUpdate(
        info, ctrl_hwnd, *cancel,
        [target_hwnd](const updater::DownloadProgress& p) {
          if (target_hwnd != nullptr && IsWindow(target_hwnd) != FALSE) {
            auto* copy = new updater::DownloadProgress(p);
            if (PostMessageW(target_hwnd, kMsgDownloadProgress, 0,
                             reinterpret_cast<LPARAM>(copy)) == FALSE) {
              delete copy;
            }
          }
        },
        &err);

    if (target_hwnd != nullptr && IsWindow(target_hwnd) != FALSE) {
      auto* done = new DownloadDonePayload{ok, err};
      if (PostMessageW(target_hwnd, kMsgDownloadDone, 0,
                       reinterpret_cast<LPARAM>(done)) == FALSE) {
        delete done;
      }
    }
  }).detach();
}

void SoftwareUpdateWindow::OpenReleaseWebpage() const {
  const wchar_t* url = updater::IsTrustedGitHubUrl(result_.release_url)
                           ? result_.release_url.c_str()
                           : kDefaultReleaseUrl;
  ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOW);
}

void SoftwareUpdateWindow::BuildButtons(int w, int h, int dpi) {
  using namespace liquid_glass;
  buttons_.clear();
  const int btn_y = h - Scale(50, dpi);
  const int btn_h = Scale(34, dpi);

  if (state_ == State::kChecking) {
    const int bw = Scale(112, dpi);
    buttons_.push_back({{(w - bw) / 2, btn_y, (w + bw) / 2, btn_y + btn_h},
                        L"隐藏", PillStyle::kSecondary, 1});
  } else if (state_ == State::kUpToDate) {
    const int bw = Scale(126, dpi);
    buttons_.push_back({{(w - bw) / 2, btn_y, (w + bw) / 2, btn_y + btn_h},
                        L"好", PillStyle::kPrimary, 1});
  } else if (state_ == State::kAvailable) {
    const int w1 = Scale(124, dpi);
    const int w2 = Scale(104, dpi);
    const int w3 = Scale(82, dpi);
    const int gap = Scale(10, dpi);
    const int total = w1 + w2 + w3 + gap * 2;
    const int x0 = (w - total) / 2;
    buttons_.push_back({{x0, btn_y, x0 + w1, btn_y + btn_h},
                        L"立即更新", PillStyle::kPrimary, 2});
    buttons_.push_back({{x0 + w1 + gap, btn_y, x0 + w1 + gap + w2, btn_y + btn_h},
                        L"前往网页", PillStyle::kSecondary, 3});
    buttons_.push_back({{x0 + w1 + w2 + gap * 2, btn_y,
                         x0 + w1 + w2 + gap * 2 + w3, btn_y + btn_h},
                        L"稍后", PillStyle::kSecondary, 1});
  } else if (state_ == State::kDownloading) {
    const int bw = Scale(128, dpi);
    buttons_.push_back({{(w - bw) / 2, btn_y, (w + bw) / 2, btn_y + btn_h},
                        L"取消下载", PillStyle::kSecondary, 4});
  } else if (state_ == State::kInstalling) {
    // 安装替换阶段无交互按钮，防止中断原子替换
  } else if (state_ == State::kError) {
    const int w1 = Scale(106, dpi);
    const int w2 = Scale(108, dpi);
    const int w3 = Scale(88, dpi);
    const int gap = Scale(10, dpi);
    const int total = w1 + w2 + w3 + gap * 2;
    const int x0 = (w - total) / 2;
    buttons_.push_back({{x0, btn_y, x0 + w1, btn_y + btn_h},
                        L"重试", PillStyle::kPrimary, 5});
    buttons_.push_back({{x0 + w1 + gap, btn_y, x0 + w1 + gap + w2, btn_y + btn_h},
                        L"前往网页", PillStyle::kSecondary, 3});
    buttons_.push_back({{x0 + w1 + w2 + gap * 2, btn_y,
                         x0 + w1 + w2 + gap * 2 + w3, btn_y + btn_h},
                        L"关闭", PillStyle::kSecondary, 1});
  }
}

void SoftwareUpdateWindow::ExecuteAction(int action_id) {
  switch (action_id) {
    case 1:
      Close();
      break;
    case 2:
      BeginDownload();
      break;
    case 3:
      OpenReleaseWebpage();
      Close();
      break;
    case 4:
      if (cancel_flag_) cancel_flag_->store(true);
      break;
    case 5:
      if (result_.has_update) {
        BeginDownload();
      } else {
        BeginCheck();
      }
      break;
    default:
      break;
  }
}

void SoftwareUpdateWindow::Paint(HDC screen_hdc) {
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

  BuildButtons(w, h, dpi);

  const int badge_w = Scale(120, dpi);
  const int badge_h = Scale(24, dpi);
  RECT badge_rc = {(w - badge_w) / 2, Scale(138, dpi), (w + badge_w) / 2,
                   Scale(138, dpi) + badge_h};

  const int card_x = Scale(22, dpi);
  const int card_y = Scale(176, dpi);
  const int card_w = w - card_x * 2;
  const int card_h = Scale(170, dpi);

  {
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

    DrawWindowBackground(g, static_cast<float>(w), static_cast<float>(h), pal_,
                         dpi);
    DrawTrafficLightClose(g, static_cast<float>(close_cx),
                          static_cast<float>(close_cy), ScaleF(6.5f, dpi),
                          hover_close_, press_close_, dpi);

    // 顶部居中图标
    const float icon_s = ScaleF(56.0f, dpi);
    DrawAppSquircleIcon(g, (static_cast<float>(w) - icon_s) * 0.5f,
                        ScaleF(40.0f, dpi), icon_s, pal_, dpi);

    const std::wstring cur_badge =
        std::wstring(L"当前版本 ") + app_version::kVersionTag;
    DrawBadgePill(g, hdc, badge_rc, cur_badge.c_str(), font_badge_, pal_.accent,
                  pal_, dpi);

    // 状态液态玻璃卡片
    DrawLiquidGlassCard(g, static_cast<float>(card_x),
                        static_cast<float>(card_y), static_cast<float>(card_w),
                        static_cast<float>(card_h), ScaleF(14.0f, dpi), pal_,
                        dpi);

    const float orb_cx = static_cast<float>(w) * 0.5f;
    const float orb_cy = static_cast<float>(card_y) + ScaleF(36.0f, dpi);
    const float orb_r = ScaleF(19.0f, dpi);

    if (state_ == State::kChecking) {
      DrawStatusOrb(g, orb_cx, orb_cy, orb_r, OrbKind::kUpdate, pal_, dpi);
      DrawProgressBar(g, static_cast<float>(card_x + Scale(32, dpi)),
                      static_cast<float>(card_y + Scale(122, dpi)),
                      static_cast<float>(card_w - Scale(64, dpi)),
                      ScaleF(6.0f, dpi), 0.0f, true, anim_phase_, pal_, dpi);
    } else if (state_ == State::kUpToDate) {
      DrawStatusOrb(g, orb_cx, orb_cy, orb_r, OrbKind::kSuccess, pal_, dpi);
    } else if (state_ == State::kAvailable) {
      DrawStatusOrb(g, orb_cx, static_cast<float>(card_y) + ScaleF(30.0f, dpi),
                    ScaleF(16.0f, dpi), OrbKind::kUpdate, pal_, dpi);
    } else if (state_ == State::kDownloading || state_ == State::kInstalling) {
      DrawStatusOrb(g, orb_cx, orb_cy, orb_r, OrbKind::kUpdate, pal_, dpi);
      const float ratio =
          (state_ == State::kInstalling)
              ? 1.0f
              : std::clamp(static_cast<float>(progress_.percent) / 100.0f, 0.0f,
                           1.0f);
      const bool indet =
          (state_ == State::kDownloading) && progress_.indeterminate;
      DrawProgressBar(g, static_cast<float>(card_x + Scale(28, dpi)),
                      static_cast<float>(card_y + Scale(108, dpi)),
                      static_cast<float>(card_w - Scale(56, dpi)),
                      ScaleF(6.5f, dpi), ratio, indet, anim_phase_, pal_, dpi);
    } else if (state_ == State::kError) {
      DrawStatusOrb(g, orb_cx, orb_cy, orb_r, OrbKind::kWarning, pal_, dpi);
    }

    // 底部按钮
    for (size_t i = 0; i < buttons_.size(); ++i) {
      const auto& b = buttons_[i];
      DrawPillButton(g, hdc, b.rc, b.label.c_str(), font_btn_, b.style,
                     static_cast<int>(i) == hover_btn_,
                     static_cast<int>(i) == press_btn_, i == 0, pal_, dpi);
    }
  }

  // 文字层
  SetBkMode(hdc, TRANSPARENT);
  SelectFont(hdc, font_caption_);
  SetTextColor(hdc, pal_.secondary);
  RECT cap_rc = {Scale(44, dpi), Scale(10, dpi), w - Scale(44, dpi),
                 Scale(30, dpi)};
  DrawTextW(hdc, L"软件更新", -1, &cap_rc,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

  SelectFont(hdc, font_app_);
  SetTextColor(hdc, pal_.text);
  RECT app_rc = {Scale(20, dpi), Scale(102, dpi), w - Scale(20, dpi),
                 Scale(132, dpi)};
  DrawTextW(hdc, L"CrossPointer", -1, &app_rc,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

  if (state_ == State::kChecking) {
    SelectFont(hdc, font_status_);
    SetTextColor(hdc, pal_.text);
    RECT t_rc = {card_x + Scale(16, dpi), card_y + Scale(64, dpi),
                 card_x + card_w - Scale(16, dpi), card_y + Scale(90, dpi)};
    DrawTextW(hdc, L"正在检查新版本...", -1, &t_rc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    SelectFont(hdc, font_small_);
    SetTextColor(hdc, pal_.secondary);
    RECT s_rc = {card_x + Scale(16, dpi), card_y + Scale(92, dpi),
                 card_x + card_w - Scale(16, dpi), card_y + Scale(112, dpi)};
    DrawTextW(hdc, L"正在安全连接 GitHub Releases 获取最新版本信息", -1, &s_rc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  } else if (state_ == State::kUpToDate) {
    SelectFont(hdc, font_status_);
    SetTextColor(hdc, pal_.text);
    RECT t_rc = {card_x + Scale(16, dpi), card_y + Scale(68, dpi),
                 card_x + card_w - Scale(16, dpi), card_y + Scale(94, dpi)};
    DrawTextW(hdc, L"您使用的是最新版本", -1, &t_rc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    SelectFont(hdc, font_body_);
    SetTextColor(hdc, pal_.secondary);
    const std::wstring d1 = std::wstring(L"CrossPointer ") +
                            app_version::kVersionText +
                            L" 是目前最新的可用版本。";
    RECT d1_rc = {card_x + Scale(16, dpi), card_y + Scale(98, dpi),
                  card_x + card_w - Scale(16, dpi), card_y + Scale(122, dpi)};
    DrawTextW(hdc, d1.c_str(), -1, &d1_rc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    SelectFont(hdc, font_small_);
    const std::wstring ts = L"最后检查：" + last_check_time_;
    RECT ts_rc = {card_x + Scale(16, dpi), card_y + Scale(126, dpi),
                  card_x + card_w - Scale(16, dpi), card_y + Scale(148, dpi)};
    DrawTextW(hdc, ts.c_str(), -1, &ts_rc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  } else if (state_ == State::kAvailable) {
    SelectFont(hdc, font_status_);
    SetTextColor(hdc, pal_.text);
    const std::wstring title = L"发现新版本 " + result_.latest_version;
    RECT t_rc = {card_x + Scale(16, dpi), card_y + Scale(52, dpi),
                 card_x + card_w - Scale(16, dpi), card_y + Scale(76, dpi)};
    DrawTextW(hdc, title.c_str(), -1, &t_rc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    SelectFont(hdc, font_small_);
    SetTextColor(hdc, pal_.secondary);
    const std::wstring preview =
        result_.changelog.empty()
            ? L"点击「立即更新」即可在应用内高速下载并原地自动重启升级。"
            : CleanChangelogPreview(result_.changelog);
    RECT p_rc = {card_x + Scale(18, dpi), card_y + Scale(80, dpi),
                 card_x + card_w - Scale(18, dpi), card_y + card_h - Scale(10, dpi)};
    DrawTextW(hdc, preview.c_str(), -1, &p_rc,
              DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
  } else if (state_ == State::kDownloading || state_ == State::kInstalling) {
    SelectFont(hdc, font_status_);
    SetTextColor(hdc, pal_.text);
    const std::wstring headline =
        (state_ == State::kInstalling)
            ? L"正在安装更新..."
            : (L"正在下载 " + result_.latest_version);
    RECT t_rc = {card_x + Scale(16, dpi), card_y + Scale(64, dpi),
                 card_x + card_w - Scale(16, dpi), card_y + Scale(90, dpi)};
    DrawTextW(hdc, headline.c_str(), -1, &t_rc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    std::wstring stats;
    if (state_ == State::kInstalling) {
      stats = progress_.stage_text.empty()
                  ? L"正在校验完整性并原地热替换，即将重启..."
                  : progress_.stage_text;
    } else if (progress_.total_bytes > 0) {
      stats = L"已下载 " + FormatBytes(progress_.downloaded_bytes) + L" / " +
              FormatBytes(progress_.total_bytes) + L" (" +
              std::to_wstring(progress_.percent) + L"%)";
      const std::wstring spd = FormatSpeed(progress_.bytes_per_sec);
      if (!spd.empty()) stats += L"  ·  " + spd;
    } else if (progress_.downloaded_bytes > 0) {
      stats = L"已下载 " + FormatBytes(progress_.downloaded_bytes);
      const std::wstring spd = FormatSpeed(progress_.bytes_per_sec);
      if (!spd.empty()) stats += L"  ·  " + spd;
    } else {
      stats = progress_.stage_text;
    }

    SelectFont(hdc, font_small_);
    SetTextColor(hdc, pal_.secondary);
    RECT s_rc = {card_x + Scale(16, dpi), card_y + Scale(124, dpi),
                 card_x + card_w - Scale(16, dpi), card_y + Scale(150, dpi)};
    DrawTextW(hdc, stats.c_str(), -1, &s_rc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  } else if (state_ == State::kError) {
    SelectFont(hdc, font_status_);
    SetTextColor(hdc, pal_.text);
    RECT t_rc = {card_x + Scale(16, dpi), card_y + Scale(66, dpi),
                 card_x + card_w - Scale(16, dpi), card_y + Scale(92, dpi)};
    DrawTextW(hdc, L"更新暂未完成", -1, &t_rc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    SelectFont(hdc, font_body_);
    SetTextColor(hdc, pal_.secondary);
    RECT e_rc = {card_x + Scale(22, dpi), card_y + Scale(96, dpi),
                 card_x + card_w - Scale(22, dpi), card_y + card_h - Scale(12, dpi)};
    DrawTextW(hdc, error_text_.c_str(), -1, &e_rc,
              DT_CENTER | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
  }

  BitBlt(screen_hdc, 0, 0, w, h, hdc, 0, 0, SRCCOPY);
  SelectObject(hdc, old_bmp);
  DeleteObject(bmp);
  DeleteDC(hdc);
}

LRESULT CALLBACK SoftwareUpdateWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp,
                                       LPARAM lp) noexcept {
  auto* self = reinterpret_cast<SoftwareUpdateWindow*>(
      GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_NCCREATE) {
    const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lp);
    self = static_cast<SoftwareUpdateWindow*>(cs->lpCreateParams);
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

LRESULT SoftwareUpdateWindow::HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
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

    case WM_TIMER: {
      if (wp == kAnimTimerId) {
        anim_phase_ += 0.024f;
        if (anim_phase_ >= 1.0f) anim_phase_ -= 1.0f;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      break;
    }

    case kMsgCheckDone: {
      std::unique_ptr<updater::UpdateResult> res(
          reinterpret_cast<updater::UpdateResult*>(lp));
      if (res) {
        result_ = *res;
        last_check_time_ = FormatNowTime();
        if (result_.has_update) {
          state_ = State::kAvailable;
        } else if (result_.success) {
          state_ = State::kUpToDate;
        } else {
          state_ = State::kError;
          error_text_ = result_.error_msg;
        }
        UpdateAnimationTimer();
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    }

    case kMsgDownloadProgress: {
      std::unique_ptr<updater::DownloadProgress> p(
          reinterpret_cast<updater::DownloadProgress*>(lp));
      if (p) {
        progress_ = *p;
        if (progress_.installing) {
          state_ = State::kInstalling;
        }
        UpdateAnimationTimer();
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    }

    case kMsgDownloadDone: {
      std::unique_ptr<DownloadDonePayload> done(
          reinterpret_cast<DownloadDonePayload*>(lp));
      if (done && !done->ok) {
        state_ = State::kError;
        error_text_ = done->error_msg.empty() ? L"更新未完成，请重试。"
                                              : done->error_msg;
        UpdateAnimationTimer();
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    }

    case WM_KEYDOWN: {
      if (wp == VK_ESCAPE && state_ != State::kInstalling) {
        Close();
        return 0;
      }
      if (wp == VK_RETURN && !buttons_.empty()) {
        ExecuteAction(buttons_[0].action_id);
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
      int hb = -1;
      for (size_t i = 0; i < buttons_.size(); ++i) {
        if (HitRect(buttons_[i].rc, mx, my)) {
          hb = static_cast<int>(i);
          break;
        }
      }
      if (hc != hover_close_ || hb != hover_btn_) {
        hover_close_ = hc;
        hover_btn_ = hb;
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    }

    case WM_MOUSELEAVE: {
      tracking_mouse_ = false;
      if (hover_close_ || hover_btn_ != -1) {
        hover_close_ = false;
        hover_btn_ = -1;
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    }

    case WM_SETCURSOR: {
      if (hover_close_ || hover_btn_ != -1) {
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
      }
      break;
    }

    case WM_LBUTTONDOWN: {
      const int mx = GET_X_LPARAM(lp);
      const int my = GET_Y_LPARAM(lp);
      if (HitRect(rc_close_, mx, my) && state_ != State::kInstalling) {
        press_close_ = true;
        SetCapture(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      for (size_t i = 0; i < buttons_.size(); ++i) {
        if (HitRect(buttons_[i].rc, mx, my)) {
          press_btn_ = static_cast<int>(i);
          SetCapture(hwnd);
          InvalidateRect(hwnd, nullptr, FALSE);
          return 0;
        }
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
      const int was_btn = press_btn_;
      press_close_ = false;
      press_btn_ = -1;
      if (was_close && HitRect(rc_close_, mx, my) &&
          state_ != State::kInstalling) {
        Close();
        return 0;
      }
      if (was_btn >= 0 && was_btn < static_cast<int>(buttons_.size()) &&
          HitRect(buttons_[static_cast<size_t>(was_btn)].rc, mx, my)) {
        ExecuteAction(buttons_[static_cast<size_t>(was_btn)].action_id);
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
      if (state_ != State::kInstalling) {
        Close();
      }
      return 0;
    }

    case WM_DESTROY: {
      hwnd_ = nullptr;
      return 0;
    }
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}
