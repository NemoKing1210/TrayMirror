#include "render/Flyout.h"

#include "render/Tooltip.h"

#include "Log.h"
#include "render/CellLayout.h"
#include "render/Theme.h"
#include "win/Monitors.h"

#include <d2d1helper.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <bit>
#include <iterator>
#include <algorithm>

namespace traymirror {
namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kFlyoutClass[] = L"TrayMirrorFlyout";

constexpr UINT_PTR kAnimationTimer = 1;

constexpr int kAnchorGapDip = 8;


UINT DpiForMonitor(HMONITOR monitor) {
  UINT dpiX = 96;
  UINT dpiY = 96;
  if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) != 0) {
    return 96;
  }
  return dpiX;
}

}  // namespace

bool Flyout::EnsureClass() {
  static const bool registered = []() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpfnWndProc = &Flyout::WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kFlyoutClass;
    if (RegisterClassExW(&wc) != 0) {
      return true;
    }
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
  }();
  return registered;
}

LRESULT CALLBACK Flyout::WndProc(HWND hwnd, UINT message, WPARAM wparam,
                                 LPARAM lparam) {
  if (message == WM_NCCREATE) {
    const auto* create = std::bit_cast<const CREATESTRUCTW*>(lparam);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(create->lpCreateParams));
  }
  auto* self = std::bit_cast<Flyout*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (self != nullptr) {
    return self->HandleMessage(hwnd, message, wparam, lparam);
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT Flyout::HandleMessage(HWND hwnd, UINT message, WPARAM wparam,
                              LPARAM lparam) {
  switch (message) {
    case WM_SETCURSOR:
      SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));
      return TRUE;
    case WM_ERASEBKGND:
      return 1;  // Paint() fills every pixel
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
    case WM_DWMCOLORIZATIONCOLORCHANGED:
      ApplyTheme();
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    case WM_PAINT: {
      PAINTSTRUCT ps = {};
      const HDC dc = BeginPaint(hwnd, &ps);
      const bool rendered = surface_ != nullptr && surface_->Render(
          [this](ID2D1DeviceContext* context, int width, int height) {
            DrawContentD2D(context, width, height);
          });
      if (!rendered) {
        Paint(dc);
      }
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_TIMER:
      TickTransition();
      return 0;
    case WM_MOUSEMOVE: {
      if (!transition_.interactive()) {
        return 0;
      }
      TrackLeave(hwnd);
      SetHover(HitTest(POINT{.x = GET_X_LPARAM(lparam),
                             .y = GET_Y_LPARAM(lparam)}));
      return 0;
    }
    case WM_MOUSEHOVER:
      if (transition_.interactive()) {
        ShowTooltip();
      }
      return 0;
    case WM_MOUSELEAVE:
      tracking_ = false;
      HideTooltip();
      if (transition_.interactive()) {
        SetHover(kNoHover);
      }
      return 0;
    case WM_LBUTTONDOWN:
      if (!transition_.interactive()) {
        return 0;
      }
      SetHover(HitTest(POINT{.x = GET_X_LPARAM(lparam),
                             .y = GET_Y_LPARAM(lparam)}));
      DispatchAt(TrayEvent::kLeftDown,
                 POINT{.x = GET_X_LPARAM(lparam), .y = GET_Y_LPARAM(lparam)});
      return 0;
    case WM_LBUTTONUP:
      if (!transition_.interactive()) {
        return 0;
      }
      DispatchAt(TrayEvent::kLeftUp,
                 POINT{.x = GET_X_LPARAM(lparam), .y = GET_Y_LPARAM(lparam)});
      Close();  // the click was the user's whole interaction
      return 0;
    case WM_LBUTTONDBLCLK:
      if (!transition_.interactive()) {
        return 0;
      }
      DispatchAt(TrayEvent::kLeftDblClk,
                 POINT{.x = GET_X_LPARAM(lparam), .y = GET_Y_LPARAM(lparam)});
      return 0;
    case WM_RBUTTONDOWN:
      if (!transition_.interactive()) {
        return 0;
      }
      DispatchAt(TrayEvent::kRightDown,
                 POINT{.x = GET_X_LPARAM(lparam), .y = GET_Y_LPARAM(lparam)});
      return 0;
    case WM_RBUTTONUP:
      if (!transition_.interactive()) {
        return 0;
      }
      DispatchAt(TrayEvent::kRightUp,
                 POINT{.x = GET_X_LPARAM(lparam), .y = GET_Y_LPARAM(lparam)});
      Close();  // the owner's menu takes over from here
      return 0;
    case WM_KEYDOWN:
      if (wparam == VK_ESCAPE) {
        Close();  // closing is idempotent, so a fading popup stays closed
      }
      return 0;
    case WM_ACTIVATE:
      if (LOWORD(wparam) == WA_INACTIVE) {
        if (activated_) {
          Close();  // an outside click deactivates the flyout
        }
      } else {
        activated_ = true;
      }
      return 0;
    case WM_CLOSE:
      Close();
      return 0;
    case WM_NCDESTROY:
      if (timerRunning_) {
        KillTimer(hwnd, kAnimationTimer);
        timerRunning_ = false;
      }
      surface_.reset();
      hwnd_ = nullptr;
      return DefWindowProcW(hwnd, message, wparam, lparam);
    default:
      break;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

std::unique_ptr<Flyout> Flyout::Create(HMONITOR monitor, const RECT& anchor,
                                       std::vector<IconView> icons,
                                       DispatchClick onClick,
                                       std::function<void()> onClose,
                                       IWICImagingFactory* wic,
                                       ID2D1Device* d2dDevice) {
  auto log = InitHostLogging("");
  if (icons.empty()) {
    return nullptr;  // nothing to reveal: the chevron would not exist
  }
  if (!EnsureClass()) {
    log->error("flyout window class registration failed ({})", GetLastError());
    return nullptr;
  }
  std::unique_ptr<Flyout> flyout(new Flyout());
  flyout->monitor_ = monitor;
  flyout->anchor_ = anchor;
  const UINT dpi = DpiForMonitor(monitor);
  flyout->iconPx_ = IconPxForDpi(dpi);
  flyout->dark_ = SystemUsesDarkTheme();
  flyout->icons_ = std::move(icons);
  flyout->onClick_ = std::move(onClick);
  flyout->onClose_ = std::move(onClose);
  flyout->wic_ = wic;

  const GridLayout grid = LayoutGrid(flyout->iconPx_, flyout->icons_.size());
  flyout->cells_ = grid.cells;
  if (grid.width <= 0 || grid.height <= 0) {
    return nullptr;
  }

  // Above the strip, right edges aligned; clamped into the work area so
  // a strip at the top edge puts the flyout below itself instead.
  MONITORINFO mi = {};
  mi.cbSize = sizeof(mi);
  const bool haveMonitor = GetMonitorInfoW(monitor, &mi) != FALSE;
  const int gap = MulDiv(kAnchorGapDip, static_cast<int>(dpi), 96);
  int x = ((anchor.left + anchor.right) / 2) - (grid.width / 2);
  int y = anchor.top - grid.height - gap;
  if (haveMonitor) {
    if (y < mi.rcWork.top) {
      y = anchor.bottom + gap;
    }
    if (x < mi.rcWork.left) {
      x = mi.rcWork.left;
    }
    if (x + grid.width > mi.rcWork.right) {
      x = mi.rcWork.right - grid.width;
    }
    if (y + grid.height > mi.rcWork.bottom) {
      y = mi.rcWork.bottom - grid.height;
    }
  }

  const HWND hwnd = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED, kFlyoutClass,
      L"TrayMirrorFlyout", WS_POPUP, x, y, grid.width, grid.height, nullptr,
      nullptr, GetModuleHandleW(nullptr), flyout.get());
  if (hwnd == nullptr) {
    log->error("flyout window creation failed ({})", GetLastError());
    return nullptr;
  }
  flyout->hwnd_ = hwnd;
  flyout->transition_ = PopupTransition(PopupTransition::Config{
      .animationsEnabled = SystemAnimationsEnabled(),
      .durationMs = PopupTransition::kDefaultDurationMs});

  flyout->surface_ = NativePopupSurface::Attach(hwnd, d2dDevice);
  if (flyout->surface_ == nullptr) {
    log->warn("flyout composition unavailable — painting the opaque fallback");
  }
  flyout->ApplyTheme();

  const auto step = flyout->transition_.Open();
  flyout->ApplyAlpha(step.alpha);
  ShowWindow(hwnd, SW_SHOW);
  // The user just clicked the chevron, so the process received the last
  // input event and is allowed to take the foreground; without it the
  // keyboard (and therefore Esc) never reaches this thread.
  SetForegroundWindow(hwnd);
  SetFocus(hwnd);
  flyout->activated_ = GetForegroundWindow() == hwnd;
  if (!flyout->activated_) {
    log->warn("flyout could not take the foreground: Esc and outside-click "
              "will not close it");
  }
  UpdateWindow(hwnd);
  if (flyout->transition_.animating()) {
    flyout->StartTimer();
  }
  return flyout;
}

Flyout::~Flyout() { CloseImmediately(); }

void Flyout::Update(std::vector<IconView> icons, const RECT& anchor) {
  if (icons.empty()) {
    Close();
    return;
  }
  const bool preserve = hovered_ < icons_.size() && hovered_ < icons.size() &&
      icons_[hovered_].target.owner == icons[hovered_].target.owner &&
      icons_[hovered_].target.uid == icons[hovered_].target.uid;
  icons_ = std::move(icons);
  PruneBitmaps();
  if (!preserve) {
    hovered_ = kNoHover;
    HideTooltip();
  }
  anchor_ = anchor;
  const GridLayout grid = LayoutGrid(iconPx_, icons_.size());
  cells_ = grid.cells;
  MONITORINFO mi = {};
  mi.cbSize = sizeof(mi);
  if (hwnd_ == nullptr || GetMonitorInfoW(monitor_, &mi) == FALSE) {
    return;
  }
  const int width = (std::min)(grid.width, static_cast<int>(mi.rcWork.right - mi.rcWork.left));
  const int height = (std::min)(grid.height, static_cast<int>(mi.rcWork.bottom - mi.rcWork.top));
  const int gap = MulDiv(kAnchorGapDip, static_cast<int>(GetDpiForWindow(hwnd_)), 96);
  int x = ((anchor.left + anchor.right) / 2) - (width / 2);
  int y = anchor.top - height - gap;
  if (y < mi.rcWork.top) {
    y = anchor.bottom + gap;
  }
  x = std::clamp(x, static_cast<int>(mi.rcWork.left), static_cast<int>(mi.rcWork.right) - width);
  y = std::clamp(y, static_cast<int>(mi.rcWork.top), static_cast<int>(mi.rcWork.bottom) - height);
  SetWindowPos(hwnd_, nullptr, x, y, width, height, SWP_NOACTIVATE | SWP_NOZORDER);
  if (surface_ != nullptr) {
    surface_->Resize(width, height);
  }
  InvalidateRect(hwnd_, nullptr, FALSE);
  // The pointer did not move, but the hovered icon's text may have
  // changed: refresh the visible tip in place.
  if (tooltip_ != nullptr && IsWindowVisible(tooltip_->hwnd()) != FALSE &&
      hovered_ != kNoHover) {
    ShowTooltip();
  }
}

void Flyout::ApplyTheme() {
  dark_ = SystemUsesDarkTheme();
  if (surface_ != nullptr) {
    surface_->ApplyTheme(dark_);
  }
}

bool Flyout::usesComposition() const noexcept {
  return surface_ != nullptr && surface_->compositionAvailable();
}

bool Flyout::materialActive() const noexcept {
  return surface_ != nullptr && surface_->materialActive();
}

bool Flyout::IsClosing() const noexcept {
  return transition_.phase() == PopupPhase::kClosing;
}

void Flyout::Close() {
  HideTooltip();  // never leave a tip floating over a fading popup
  if (hwnd_ == nullptr) {
    // Already gone: the completion callback still has to run once.
    auto close = std::move(onClose_);
    if (close) {
      close();
    }
    return;
  }
  const auto step = transition_.Close();
  ApplyStep(step);
  if (step.command == PopupCommand::kDestroy) {
    return;  // ApplyStep destroyed the window (and may have run onClose)
  }
  if (transition_.animating()) {
    StartTimer();
  }
}

bool Flyout::Reopen() {
  if (hwnd_ == nullptr) {
    return false;
  }
  ApplyStep(transition_.Open());
  if (transition_.animating()) {
    StartTimer();
  }
  return transition_.visible();
}

void Flyout::CloseImmediately() {
  transition_.FinishImmediately();
  DestroyNow();
}

void Flyout::ApplyStep(const PopupTransition::Step& step) {
  ApplyAlpha(step.alpha);
  if (step.command == PopupCommand::kDestroy) {
    DestroyNow();
    return;
  }
  if (!transition_.animating() && timerRunning_ && hwnd_ != nullptr) {
    KillTimer(hwnd_, kAnimationTimer);
    timerRunning_ = false;
  }
}

void Flyout::ApplyAlpha(int alpha) {
  if (hwnd_ == nullptr) {
    return;
  }
  SetLayeredWindowAttributes(hwnd_, 0, static_cast<BYTE>(alpha), LWA_ALPHA);
}

void Flyout::TickTransition() {
  if (!timerRunning_ || hwnd_ == nullptr) {
    return;
  }
  const ULONGLONG now = GetTickCount64();
  const auto elapsed =
      static_cast<int>((std::min)(now - tickMs_, 1000ULL));
  tickMs_ = now;
  ApplyStep(transition_.Advance(elapsed));
}

void Flyout::StartTimer() {
  if (hwnd_ == nullptr || timerRunning_) {
    return;
  }
  tickMs_ = GetTickCount64();
  if (SetTimer(hwnd_, kAnimationTimer,
               static_cast<UINT>(PopupTransition::kIntervalMs), nullptr) != 0) {
    timerRunning_ = true;
    return;
  }
  // A timer failure must not leave the popup frozen at alpha zero: skip
  // straight to the target instead.
  ApplyStep(transition_.Advance(PopupTransition::kDefaultDurationMs * 2));
}

void Flyout::DestroyNow() {
  if (timerRunning_ && hwnd_ != nullptr) {
    KillTimer(hwnd_, kAnimationTimer);
  }
  timerRunning_ = false;
  auto close = std::move(onClose_);
  onClick_ = {};
  if (hwnd_ != nullptr) {
    surface_.reset();  // the composition target dies with the window
    tooltip_.reset();
    const HWND hwnd = hwnd_;
    hwnd_ = nullptr;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    DestroyWindow(hwnd);
  }
  if (close) {
    close();  // may destroy this object: nothing below may use a member
  }
}

void Flyout::Paint(HDC dc) {
  RECT client = {};
  GetClientRect(hwnd_, &client);
  const ThemeTokens tokens = ThemeTokens::Current(dark_);
  // GDI is only the composition-unavailable fallback, so it is always the
  // opaque system surface; GDI over the acrylic material is additive and
  // black-keyed and cannot be composited correctly.
  auto* const brush = static_cast<HBRUSH>(GetStockObject(DC_BRUSH));
  SetDCBrushColor(dc, tokens.surface);
  FillRect(dc, &client, brush);
  if (hovered_ != kNoHover && hovered_ < cells_.size()) {
    const int dpi =
        hwnd_ != nullptr ? static_cast<int>(GetDpiForWindow(hwnd_)) : 96;
    // The same rule as the strip: the full cell is the hit region, the
    // highlight is a DPI-scaled visual inset, never a second padding.
    const RECT hover = HighlightBounds(cells_[hovered_], dpi);
    const auto channel = [alpha = tokens.hover.a](BYTE base, float top) {
      return static_cast<BYTE>((static_cast<float>(base) * (1.0F - alpha)) +
                               (255.0F * top * alpha));
    };
    SetDCBrushColor(dc, RGB(channel(GetRValue(tokens.surface), tokens.hover.r),
                           channel(GetGValue(tokens.surface), tokens.hover.g),
                           channel(GetBValue(tokens.surface), tokens.hover.b)));
    const HGDIOBJ previousBrush = SelectObject(dc, brush);
    const HGDIOBJ previousPen = SelectObject(dc, GetStockObject(NULL_PEN));
    const int radius = MulDiv(metrics::kControlCornerRadius, dpi, 96);
    RoundRect(dc, hover.left, hover.top, hover.right, hover.bottom, radius, radius);
    SelectObject(dc, previousPen);
    SelectObject(dc, previousBrush);
  }
  for (size_t i = 0; i < cells_.size() && i < icons_.size(); ++i) {
    const RECT& cell = cells_[i];
    const int x = cell.left + ((cell.right - cell.left - iconPx_) / 2);
    const int y = cell.top + ((cell.bottom - cell.top - iconPx_) / 2);
    const HBITMAP bitmap = BitmapFor(icons_[i]);
    if (bitmap != nullptr && BlendBitmap(dc, bitmap, x, y, iconPx_)) {
      continue;
    }
    // DrawIconEx is the same native compositor this conversion is tested
    // against, so a failed conversion degrades to correct pixels, not a
    // blank cell.
    DrawIconEx(dc, x, y, icons_[i].icon, iconPx_, iconPx_, 0,
               nullptr, DI_NORMAL);
  }
}

void Flyout::DrawContentD2D(ID2D1DeviceContext* dc,
                            [[maybe_unused]] int width,
                            [[maybe_unused]] int height) {
  if (dc == nullptr) {
    return;
  }
  const ThemeTokens tokens = ThemeTokens::Current(dark_);
  if (hovered_ != kNoHover && hovered_ < cells_.size()) {
    const int dpi =
        hwnd_ != nullptr ? static_cast<int>(GetDpiForWindow(hwnd_)) : 96;
    const RECT hover = HighlightBounds(cells_[hovered_], dpi);
    const auto radius =
        static_cast<float>(MulDiv(metrics::kControlCornerRadius, dpi, 96));
    ComPtr<ID2D1SolidColorBrush> brush;
    if (SUCCEEDED(dc->CreateSolidColorBrush(tokens.hover,
                                            brush.GetAddressOf()))) {
      const D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(
          D2D1::RectF(static_cast<float>(hover.left),
                      static_cast<float>(hover.top),
                      static_cast<float>(hover.right),
                      static_cast<float>(hover.bottom)),
          radius, radius);
      dc->FillRoundedRectangle(&rounded, brush.Get());
    }
  }
  for (size_t i = 0; i < cells_.size() && i < icons_.size(); ++i) {
    const RECT& cell = cells_[i];
    const int x = cell.left + ((cell.right - cell.left - iconPx_) / 2);
    const int y = cell.top + ((cell.bottom - cell.top - iconPx_) / 2);
    ID2D1Bitmap* bitmap = D2dBitmapFor(dc, icons_[i]);
    if (bitmap == nullptr) {
      continue;  // no GDI fallback over the material
    }
    const D2D1_RECT_F destination =
        D2D1::RectF(static_cast<float>(x), static_cast<float>(y),
                    static_cast<float>(x + iconPx_),
                    static_cast<float>(y + iconPx_));
    dc->DrawBitmap(bitmap, destination, 1.0F,
                   D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
  }
}

ID2D1Bitmap* Flyout::D2dBitmapFor(ID2D1DeviceContext* dc,
                                  const IconView& view) {
  if (dc == nullptr || wic_ == nullptr || view.icon == nullptr) {
    return nullptr;
  }
  CachedIcon* entry = nullptr;
  for (CachedIcon& candidate : bitmaps_) {
    if (candidate.icon == view.icon && candidate.serial == view.serial &&
        candidate.px == iconPx_) {
      entry = &candidate;
      break;
    }
  }
  if (entry == nullptr) {
    bitmaps_.push_back(CachedIcon{
        .icon = view.icon, .serial = view.serial, .px = iconPx_});
    entry = &bitmaps_.back();
  }
  if (entry->d2d != nullptr) {
    return entry->d2d.Get();
  }
  RasterIcon raster;
  if (!RasterizeIcon(wic_, view.icon, iconPx_, &raster)) {
    return nullptr;
  }
  const D2D1_BITMAP_PROPERTIES properties = D2D1::BitmapProperties(
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                        D2D1_ALPHA_MODE_PREMULTIPLIED),
      96.0F, 96.0F);
  ComPtr<ID2D1Bitmap> bitmap;
  if (FAILED(dc->CreateBitmap(
          D2D1::SizeU(static_cast<UINT32>(raster.width),
                      static_cast<UINT32>(raster.height)),
          raster.pixels.data(),
          static_cast<UINT32>(raster.width) * sizeof(uint32_t), &properties,
          bitmap.GetAddressOf()))) {
    return nullptr;
  }
  entry->d2d = std::move(bitmap);
  return entry->d2d.Get();
}

HBITMAP Flyout::BitmapFor(const IconView& view) {
  if (wic_ == nullptr || view.icon == nullptr) {
    return nullptr;
  }
  for (const CachedIcon& entry : bitmaps_) {
    if (entry.icon == view.icon && entry.serial == view.serial &&
        entry.px == iconPx_) {
      return entry.bitmap.get();
    }
  }
  UniqueBitmap bitmap = CreatePremultipliedBitmap(wic_, view.icon, iconPx_);
  if (bitmap == nullptr) {
    return nullptr;
  }
  const HBITMAP raw = bitmap.get();
  bitmaps_.push_back(CachedIcon{.icon = view.icon,
                                .serial = view.serial,
                                .px = iconPx_,
                                .bitmap = std::move(bitmap)});
  return raw;
}

void Flyout::PruneBitmaps() {
  std::erase_if(bitmaps_, [this](const CachedIcon& entry) {
    return std::ranges::none_of(icons_, [&entry](const IconView& view) {
      return view.icon == entry.icon && view.serial == entry.serial;
    });
  });
}

void Flyout::SetHover(size_t index) {
  if (index == hovered_) {
    return;
  }
  hovered_ = index;
  HideTooltip();  // a different cell is a different tip
  if (index != kNoHover) {
    ArmHover();
  }
  if (hwnd_ != nullptr) {
    InvalidateRect(hwnd_, nullptr, FALSE);
  }
}

size_t Flyout::HitTest(POINT pt) const {
  for (size_t i = 0; i < cells_.size(); ++i) {
    const RECT& cell = cells_[i];
    if (pt.x >= cell.left && pt.x < cell.right && pt.y >= cell.top &&
        pt.y < cell.bottom) {
      return i;
    }
  }
  return kNoHover;
}

void Flyout::DispatchAt(TrayEvent event, POINT pt) {
  const size_t index = HitTest(pt);
  if (index == kNoHover || index >= icons_.size() || !onClick_) {
    return;
  }
  const DispatchTarget& target = icons_[index].target;
  if (target.owner == nullptr) {
    return;
  }
  RECT window = {};
  if (GetWindowRect(hwnd_, &window) == FALSE) {
    return;
  }
  onClick_(DispatchCall{
      .target = target,
      .event = event,
      .screenPos = POINT{.x = window.left + pt.x, .y = window.top + pt.y}});
}

void Flyout::TrackLeave(HWND hwnd) {
  if (tracking_) {
    return;
  }
  TRACKMOUSEEVENT track = {};
  track.cbSize = sizeof(track);
  track.dwFlags = TME_LEAVE;
  track.hwndTrack = hwnd;
  TrackMouseEvent(&track);
  tracking_ = true;
}

void Flyout::ArmHover() {
  if (hwnd_ == nullptr) {
    return;
  }
  TRACKMOUSEEVENT track = {};
  track.cbSize = sizeof(track);
  track.dwFlags = TME_HOVER;
  track.hwndTrack = hwnd_;
  track.dwHoverTime = HOVER_DEFAULT;
  TrackMouseEvent(&track);
}

void Flyout::ShowTooltip() {
  if (hwnd_ == nullptr || !transition_.interactive() ||
      hovered_ >= icons_.size() || hovered_ >= cells_.size()) {
    return;
  }
  if (icons_[hovered_].tip.empty()) {
    HideTooltip();
    return;
  }
  if (tooltip_ == nullptr) {
    tooltip_ = Tooltip::Create(hwnd_);
  }
  if (tooltip_ == nullptr) {
    return;
  }
  RECT window = {};
  if (GetWindowRect(hwnd_, &window) == FALSE) {
    return;
  }
  const RECT& cell = cells_[hovered_];
  const RECT anchor = {.left = window.left + cell.left,
                       .top = window.top + cell.top,
                       .right = window.left + cell.right,
                       .bottom = window.top + cell.bottom};
  tooltip_->Show(icons_[hovered_].tip, anchor);
}

void Flyout::HideTooltip() {
  if (tooltip_ != nullptr) {
    tooltip_->Hide();
  }
}

}  // namespace traymirror
