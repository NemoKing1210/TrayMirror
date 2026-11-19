#include "render/Overlay.h"

#include "Log.h"
#include "render/CellLayout.h"
#include "win/Monitors.h"

#include <d2d1helper.h>
#include <windowsx.h>

#include <bit>
#include <iterator>
#include <set>
#include <utility>

namespace traymirror {
namespace {

constexpr wchar_t kOverlayClass[] = L"TrayMirrorOverlay";

// Posted by an open flyout after it destroys itself: the reset happens
// outside the flyout's own message handler, so no object dies mid-frame.
constexpr UINT kMsgFlyoutClosed = WM_APP + 4;

// Solid taskbar colors used until the stage 2 visual pass pins the exact
// system values (spec 6.3): the Win11 light and dark taskbar tones.
constexpr D2D1_COLOR_F kLightBackground = {
    .r = 0.953F, .g = 0.953F, .b = 0.953F, .a = 1.0F};  // #F3F3F3
constexpr D2D1_COLOR_F kDarkBackground = {
    .r = 0.125F, .g = 0.125F, .b = 0.125F, .a = 1.0F};  // #202020

bool SystemUsesDarkTheme() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(
          HKEY_CURRENT_USER,
          L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
          0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
    return true;  // the Win11 default taskbar is dark
  }
  DWORD light = 1;
  DWORD size = sizeof(light);
  DWORD type = 0;
  const LONG status = RegQueryValueExW(
      key, L"AppsUseLightTheme", nullptr, &type,
      reinterpret_cast<LPBYTE>(&light), &size);
  RegCloseKey(key);
  if (status != ERROR_SUCCESS || type != REG_DWORD) {
    return true;
  }
  return light == 0;
}

}  // namespace

bool Overlay::EnsureClass() {
  static const bool registered = []() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;  // without it WM_LBUTTONDBLCLK never arrives
    wc.lpfnWndProc = &Overlay::WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kOverlayClass;
    if (RegisterClassExW(&wc) != 0) {
      return true;
    }
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
  }();
  return registered;
}

LRESULT CALLBACK Overlay::WndProc(HWND hwnd, UINT message, WPARAM wparam,
                                  LPARAM lparam) {
  if (message == WM_NCCREATE) {
    const auto* create = std::bit_cast<const CREATESTRUCTW*>(lparam);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(create->lpCreateParams));
  }
  auto* self = std::bit_cast<Overlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (self != nullptr) {
    return self->HandleMessage(hwnd, message, wparam, lparam);
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT Overlay::HandleMessage(HWND hwnd, UINT message, WPARAM wparam,
                               LPARAM lparam) {
  // `hwnd` is the parameter, not hwnd_: during WM_NCCREATE the member is
  // not assigned yet, and DefWindowProc(nullptr, ...) would refuse the
  // creation.
  switch (message) {
    case WM_ERASEBKGND:
      return 1;  // the composition surface owns the pixels
    case WM_PAINT:
      ValidateRect(hwnd, nullptr);
      return 0;
    case WM_MOUSEMOVE: {
      const POINT pt = {.x = GET_X_LPARAM(lparam), .y = GET_Y_LPARAM(lparam)};
      HandleMouseMove(hwnd, pt);
      DispatchGesture(TrayEvent::kMouseMove, pt);
      return 0;
    }
    case WM_LBUTTONDOWN:
      DispatchGesture(TrayEvent::kLeftDown,
                      POINT{.x = GET_X_LPARAM(lparam),
                            .y = GET_Y_LPARAM(lparam)});
      return 0;
    case WM_LBUTTONUP: {
      const POINT pt = {.x = GET_X_LPARAM(lparam), .y = GET_Y_LPARAM(lparam)};
      if (HandleChevronClick(pt)) {
        return 0;  // the chevron is not an icon: nothing is dispatched
      }
      DispatchGesture(TrayEvent::kLeftUp, pt);
      return 0;
    }
    case WM_LBUTTONDBLCLK:
      DispatchGesture(TrayEvent::kLeftDblClk,
                      POINT{.x = GET_X_LPARAM(lparam),
                            .y = GET_Y_LPARAM(lparam)});
      return 0;
    case WM_RBUTTONDOWN:
      DispatchGesture(TrayEvent::kRightDown,
                      POINT{.x = GET_X_LPARAM(lparam),
                            .y = GET_Y_LPARAM(lparam)});
      return 0;
    case WM_RBUTTONUP:
      DispatchGesture(TrayEvent::kRightUp,
                      POINT{.x = GET_X_LPARAM(lparam),
                            .y = GET_Y_LPARAM(lparam)});
      return 0;
    case WM_MOUSELEAVE:
      DispatchLeave();
      ClearHover();
      return 0;
    case kMsgFlyoutClosed:
      flyout_.reset();  // its window is already gone
      return 0;
    case WM_SETTINGCHANGE:
      ApplyTheme();
      dirty_ = true;
      Render();
      return 0;
    case WM_DPICHANGED:
      // The strip geometry depends on the clock and the taskbar, so the
      // owner recomputes it (spec 6.1: reposition on events, not a timer).
      if (onGeometryInvalidated_) {
        onGeometryInvalidated_();
      }
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

std::unique_ptr<Overlay> Overlay::Create(
    HMONITOR monitor, const RECT& strip,
    std::function<void()> onGeometryInvalidated, DispatchClick onClick) {
  auto log = InitHostLogging("");
  const int width = strip.right - strip.left;
  const int height = strip.bottom - strip.top;
  if (width <= 0 || height <= 0) {
    log->warn("overlay strip is empty ({},{},{},{}) — not created",
              strip.left, strip.top, strip.right, strip.bottom);
    return nullptr;
  }
  if (!EnsureClass()) {
    log->error("overlay window class registration failed ({})", GetLastError());
    return nullptr;
  }

  std::unique_ptr<Overlay> overlay(new Overlay());
  overlay->monitor_ = monitor;
  overlay->strip_ = strip;
  overlay->onGeometryInvalidated_ = std::move(onGeometryInvalidated);
  overlay->onClick_ = std::move(onClick);

  const HWND hwnd = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, kOverlayClass,
      L"TrayMirrorOverlay", WS_POPUP, strip.left, strip.top, width, height,
      nullptr, nullptr, GetModuleHandleW(nullptr), overlay.get());
  if (hwnd == nullptr) {
    log->error("overlay window creation failed ({})", GetLastError());
    return nullptr;
  }
  overlay->hwnd_ = hwnd;
  overlay->ApplyTheme();
  if (!overlay->InitComposition()) {
    // Degraded, not fatal: the window stays alive and the host keeps
    // modelling, so a GPU-less remote session still logs the tray.
    log->warn("overlay composition unavailable — strip renders nothing");
  }
  ShowWindow(hwnd, SW_SHOWNOACTIVATE);
  overlay->Render();
  return overlay;
}

Overlay::~Overlay() {
  if (flyout_ != nullptr) {
    flyout_->Close();  // its window must die before ours
  }
  ReleaseComposition();
  if (hwnd_ != nullptr) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
}

void Overlay::SetIcons(std::vector<IconView> icons,
                       std::vector<IconView> overflow) {
  if (flyout_ != nullptr) {
    CloseFlyout();  // the model changed under an open flyout
  }
  icons_ = std::move(icons);
  overflow_ = std::move(overflow);
  dirty_ = true;
}

void Overlay::ApplyTheme() { dark_ = SystemUsesDarkTheme(); }

bool Overlay::InitComposition() {
  auto log = InitHostLogging("");
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0,
                                      D3D_FEATURE_LEVEL_10_1,
                                      D3D_FEATURE_LEVEL_10_0};
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                 static_cast<UINT>(std::size(levels)),
                                 D3D11_SDK_VERSION, d3d_.GetAddressOf(), nullptr,
                                 nullptr);
  if (FAILED(hr) != 0) {
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                           D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                           static_cast<UINT>(std::size(levels)),
                           D3D11_SDK_VERSION, d3d_.GetAddressOf(), nullptr,
                           nullptr);
    if (FAILED(hr) != 0) {
      log->warn("D3D11CreateDevice failed ({:#x})", static_cast<unsigned>(hr));
      return false;
    }
  }

  Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
  hr = d3d_.As(&dxgiDevice);
  if (FAILED(hr) != 0) {
    return false;
  }
  hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                         d2dFactory_.GetAddressOf());
  if (FAILED(hr) != 0) {
    return false;
  }
  hr = d2dFactory_->CreateDevice(dxgiDevice.Get(), d2dDevice_.GetAddressOf());
  if (FAILED(hr) != 0) {
    return false;
  }
  hr = d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                                       d2dContext_.GetAddressOf());
  if (FAILED(hr) != 0) {
    return false;
  }
  // The D2D device is the rendering device, which is what makes
  // BeginDraw able to hand back an ID2D1DeviceContext.
  hr = DCompositionCreateDevice2(
      d2dDevice_.Get(), IID_PPV_ARGS(dcomp_.GetAddressOf()));
  if (FAILED(hr) != 0) {
    log->warn("DCompositionCreateDevice2 failed ({:#x})",
              static_cast<unsigned>(hr));
    return false;
  }
  hr = dcomp_->CreateTargetForHwnd(hwnd_, TRUE, dcompTarget_.GetAddressOf());
  if (FAILED(hr) != 0) {
    return false;
  }
  hr = dcomp_->CreateVisual(dcompVisual_.GetAddressOf());
  if (FAILED(hr) != 0) {
    return false;
  }
  hr = dcompTarget_->SetRoot(dcompVisual_.Get());
  if (FAILED(hr) != 0) {
    return false;
  }
  hr = dcomp_->CreateSurface(
      static_cast<UINT>(strip_.right - strip_.left),
      static_cast<UINT>(strip_.bottom - strip_.top), DXGI_FORMAT_B8G8R8A8_UNORM,
      DXGI_ALPHA_MODE_PREMULTIPLIED, surface_.GetAddressOf());
  if (FAILED(hr) != 0) {
    log->warn("CreateSurface failed ({:#x})", static_cast<unsigned>(hr));
    return false;
  }
  hr = dcompVisual_->SetContent(surface_.Get());
  if (FAILED(hr) != 0) {
    return false;
  }
  hr = dcomp_->Commit();
  return FAILED(hr) == 0;
}

void Overlay::ReleaseComposition() {
  surface_.Reset();
  dcompVisual_.Reset();
  dcompTarget_.Reset();
  dcomp_.Reset();
  d2dContext_.Reset();
  d2dDevice_.Reset();
  d2dFactory_.Reset();
  d3d_.Reset();
}

void Overlay::Render() {
  if (!dirty_) {
    return;
  }
  dirty_ = false;
  if (surface_ == nullptr || dcomp_ == nullptr) {
    return;  // degraded: composition unavailable
  }
  auto log = InitHostLogging("");
  const int width = strip_.right - strip_.left;
  const int height = strip_.bottom - strip_.top;
  RECT update = {};
  update.right = width;
  update.bottom = height;
  POINT offset = {};
  Microsoft::WRL::ComPtr<ID2D1DeviceContext> dc;
  HRESULT hr = surface_->BeginDraw(&update, IID_PPV_ARGS(dc.GetAddressOf()),
                                   &offset);
  if (FAILED(hr) != 0 || dc == nullptr) {
    log->warn("surface BeginDraw failed ({:#x})", static_cast<unsigned>(hr));
    return;
  }
  dc->SetTransform(D2D1::Matrix3x2F::Translation(
      static_cast<float>(offset.x), static_cast<float>(offset.y)));
  const D2D1_COLOR_F background = dark_ ? kDarkBackground : kLightBackground;
  dc->Clear(&background);
  DrawIcons(dc.Get(), width, height);
  hr = surface_->EndDraw();
  if (FAILED(hr) != 0) {
    log->warn("surface EndDraw failed ({:#x})", static_cast<unsigned>(hr));
    return;
  }
  hr = dcomp_->Commit();
  if (FAILED(hr) != 0) {
    log->warn("composition commit failed ({:#x})", static_cast<unsigned>(hr));
  }
}

void Overlay::HandleMouseMove(HWND hwnd, POINT pt) {
  if (!tracking_) {
    TRACKMOUSEEVENT track = {};
    track.cbSize = sizeof(track);
    track.dwFlags = TME_LEAVE;
    track.hwndTrack = hwnd;
    TrackMouseEvent(&track);
    tracking_ = true;
  }
  size_t index = kNoHover;
  if (!HitTestIcon(cells_, pt, &index)) {
    // The chevron is a cell of the strip but not an icon: it highlights
    // without a tooltip and without a gesture to any owner.
    const bool onChevron = hasChevron_ && PtInRect(&chevronCell_, pt) != 0;
    if (chevronHovered_ != onChevron) {
      chevronHovered_ = onChevron;
      dirty_ = true;
      Render();
    }
    if (hovered_ != kNoHover) {
      DispatchLeave();  // the pointer left the icon for the gap
    }
    ClearHover();
    return;
  }
  if (chevronHovered_) {
    chevronHovered_ = false;
    dirty_ = true;
    Render();
  }
  if (index == hovered_) {
    return;
  }
  if (hovered_ != kNoHover) {
    DispatchLeave();  // the pointer left the previously hovered cell
  }
  hovered_ = index;
  dirty_ = true;
  Render();
  if (index >= icons_.size()) {
    return;
  }
  if (tooltip_ == nullptr) {
    tooltip_ = Tooltip::Create(hwnd);
  }
  if (tooltip_ != nullptr && icons_[index].tip[0] != L'\0') {
    // The tooltip places itself above the icon's screen rect (spec 6.4).
    const RECT& cell = cells_[index];
    const RECT iconRect = {.left = strip_.left + cell.left,
                           .top = strip_.top + cell.top,
                           .right = strip_.left + cell.right,
                           .bottom = strip_.top + cell.bottom};
    tooltip_->Show(icons_[index].tip, iconRect);
  } else if (tooltip_ != nullptr) {
    tooltip_->Hide();
  }
  // The pointer entered this cell: v4 owners get NIN_POPUPOPEN (report 07).
  DispatchGesture(TrayEvent::kMouseEnter, pt);
}

bool Overlay::HandleChevronClick(POINT pt) {
  if (!hasChevron_ || PtInRect(&chevronCell_, pt) == 0) {
    return false;
  }
  ToggleFlyout();
  return true;
}

void Overlay::ToggleFlyout() {
  if (flyout_ != nullptr) {
    CloseFlyout();
    return;
  }
  if (overflow_.empty() || hwnd_ == nullptr) {
    return;
  }
  // The owner is told after the window is gone; the reset below happens
  // on the message loop, never inside the flyout's own handler.
  flyout_ = Flyout::Create(monitor_, strip_, overflow_, onClick_, [this]() {
    if (hwnd_ != nullptr) {
      PostMessageW(hwnd_, kMsgFlyoutClosed, 0, 0);
    }
  });
}

void Overlay::CloseFlyout() {
  if (flyout_ == nullptr) {
    return;
  }
  if (flyout_->hwnd() != nullptr) {
    flyout_->Close();  // posts kMsgFlyoutClosed; the window is gone now
  } else {
    flyout_.reset();  // already closed; its post is still in flight
  }
}

void Overlay::DispatchGesture(TrayEvent event, POINT pt) {
  if (!onClick_) {
    return;
  }
  size_t index = kNoHover;
  if (!HitTestIcon(cells_, pt, &index) || index >= icons_.size()) {
    return;
  }
  const DispatchTarget& target = icons_[index].target;
  if (target.owner == nullptr) {
    return;
  }
  onClick_(DispatchCall{
      .target = target,
      .event = event,
      .screenPos = POINT{.x = strip_.left + pt.x, .y = strip_.top + pt.y}});
}

void Overlay::DispatchLeave() {
  if (!onClick_ || hovered_ >= icons_.size() || hovered_ >= cells_.size()) {
    return;
  }
  const DispatchTarget& target = icons_[hovered_].target;
  if (target.owner == nullptr) {
    return;
  }
  const RECT& cell = cells_[hovered_];
  onClick_(DispatchCall{
      .target = target,
      .event = TrayEvent::kMouseLeave,
      .screenPos = POINT{.x = strip_.left + cell.left +
                              ((cell.right - cell.left) / 2),
                         .y = strip_.top + cell.top +
                              ((cell.bottom - cell.top) / 2)}});
}

void Overlay::ClearHover() {
  tracking_ = false;
  if (tooltip_ != nullptr) {
    tooltip_->Hide();
  }
  if (hovered_ == kNoHover) {
    return;
  }
  hovered_ = kNoHover;
  dirty_ = true;
  Render();
}

bool HitTestIcon(const std::vector<RECT>& cells, POINT pt, size_t* index) {
  for (size_t i = 0; i < cells.size(); ++i) {
    const RECT& cell = cells[i];
    if (pt.x >= cell.left && pt.x < cell.right && pt.y >= cell.top &&
        pt.y < cell.bottom) {
      if (index != nullptr) {
        *index = i;
      }
      return true;
    }
  }
  return false;
}

void Overlay::DrawIcons(ID2D1DeviceContext* dc, int width, int height) {
  UINT dpi = GetDpiForWindow(hwnd_);
  if (dpi == 0) {
    dpi = 96;
  }
  const int iconPx = IconPxForDpi(dpi);
  const int gap = IconGapPxForDpi(dpi);
  const StripLayout layout =
      LayoutStrip(width, height, iconPx, gap, icons_.size(), !overflow_.empty());
  cells_ = layout.icons;
  hasChevron_ = layout.hasChevron;
  chevronCell_ = layout.chevron;
  std::set<IconCacheKey> live;
  for (size_t i = 0; i < cells_.size(); ++i) {
    const IconView& icon = icons_[i];
    const RECT& rect = cells_[i];
    const D2D1_RECT_F cell =
        D2D1::RectF(static_cast<float>(rect.left), static_cast<float>(rect.top),
                    static_cast<float>(rect.right),
                    static_cast<float>(rect.bottom));
    ID2D1Bitmap* bitmap = cache_.Get(icon.icon, icon.serial, iconPx);
    live.insert(
        IconCacheKey{.icon = icon.icon, .serial = icon.serial, .px = iconPx});
    if (i == hovered_) {
      DrawHighlight(dc, cell);
    }
    if (bitmap != nullptr) {
      dc->DrawBitmap(bitmap, cell, 1.0F, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else {
      DrawPlaceholder(dc, cell);
    }
  }
  if (hasChevron_) {
    const D2D1_RECT_F cell = D2D1::RectF(
        static_cast<float>(chevronCell_.left),
        static_cast<float>(chevronCell_.top),
        static_cast<float>(chevronCell_.right),
        static_cast<float>(chevronCell_.bottom));
    if (chevronHovered_) {
      DrawHighlight(dc, cell);
    }
    DrawChevron(dc, cell);
  }
  cache_.Prune(live);
}

void Overlay::DrawHighlight(ID2D1DeviceContext* dc,
                            const D2D1_RECT_F& cell) const {
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
  const D2D1_COLOR_F color =
      dark_ ? D2D1_COLOR_F{.r = 1.0F, .g = 1.0F, .b = 1.0F, .a = 0.10F}
            : D2D1_COLOR_F{.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 0.08F};
  if (FAILED(dc->CreateSolidColorBrush(color, brush.GetAddressOf())) != 0 ||
      brush == nullptr) {
    return;
  }
  dc->FillRoundedRectangle(D2D1::RoundedRect(cell, 4.0F, 4.0F), brush.Get());
}

void Overlay::DrawChevron(ID2D1DeviceContext* dc,
                          const D2D1_RECT_F& cell) const {
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
  const D2D1_COLOR_F color =
      dark_ ? D2D1_COLOR_F{.r = 0.95F, .g = 0.95F, .b = 0.95F, .a = 0.85F}
            : D2D1_COLOR_F{.r = 0.10F, .g = 0.10F, .b = 0.10F, .a = 0.75F};
  if (FAILED(dc->CreateSolidColorBrush(color, brush.GetAddressOf())) != 0 ||
      brush == nullptr) {
    return;
  }
  const float midX = (cell.left + cell.right) / 2.0F;
  const float midY = (cell.top + cell.bottom) / 2.0F;
  const float half = (cell.right - cell.left) / 4.0F;
  const float tenth = (cell.right - cell.left) / 10.0F;
  const float stroke = tenth > 1.0F ? tenth : 1.0F;
  // A "^" pointing up: the flyout opens above the strip.
  dc->DrawLine(D2D1::Point2F(midX - half, midY + (half / 2.0F)),
               D2D1::Point2F(midX, midY - (half / 2.0F)), brush.Get(), stroke);
  dc->DrawLine(D2D1::Point2F(midX, midY - (half / 2.0F)),
               D2D1::Point2F(midX + half, midY + (half / 2.0F)), brush.Get(),
               stroke);
}

void Overlay::DrawPlaceholder(ID2D1DeviceContext* dc,
                              const D2D1_RECT_F& cell) {
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
  const D2D1_COLOR_F color = {.r = 0.5F, .g = 0.5F, .b = 0.5F, .a = 0.55F};
  if (FAILED(dc->CreateSolidColorBrush(color, brush.GetAddressOf())) != 0 ||
      brush == nullptr) {
    return;
  }
  dc->FillRoundedRectangle(D2D1::RoundedRect(cell, 2.0F, 2.0F), brush.Get());
}

ID2D1Bitmap* Overlay::IconBitmap(HICON icon, int px) const {
  // An HICON becomes pixels through a 32-bpp DIB section; DrawIconEx
  // gives straight alpha, and D2D wants premultiplied, so the buffer is
  // premultiplied in place before the bitmap is created.
  if (d2dContext_ == nullptr || icon == nullptr || px <= 0) {
    return nullptr;
  }
  BITMAPV5HEADER header = {};
  header.bV5Size = sizeof(header);
  header.bV5Width = px;
  header.bV5Height = -px;  // top-down
  header.bV5Planes = 1;
  header.bV5BitCount = 32;
  header.bV5Compression = BI_BITFIELDS;
  header.bV5RedMask = 0x00FF0000U;
  header.bV5GreenMask = 0x0000FF00U;
  header.bV5BlueMask = 0x000000FFU;
  header.bV5AlphaMask = 0xFF000000U;

  const HDC screen = GetDC(nullptr);
  if (screen == nullptr) {
    return nullptr;
  }
  void* bits = nullptr;
  const HBITMAP dib =
      CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header),
                       DIB_RGB_COLORS, &bits, nullptr, 0);
  const HDC memory = CreateCompatibleDC(screen);
  HGDIOBJ previous = nullptr;
  if (dib != nullptr && memory != nullptr && bits != nullptr) {
    previous = SelectObject(memory, dib);
    PatBlt(memory, 0, 0, px, px, BLACKNESS);
    DrawIconEx(memory, 0, 0, icon, px, px, 0, nullptr, DI_NORMAL);
  }
  if (previous != nullptr) {
    SelectObject(memory, previous);
  }
  if (memory != nullptr) {
    DeleteDC(memory);
  }
  ReleaseDC(nullptr, screen);

  Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
  if (dib != nullptr && bits != nullptr) {
    auto* pixels = static_cast<uint32_t*>(bits);
    const int count = px * px;
    for (int i = 0; i < count; ++i) {
      const uint32_t argb = pixels[i];
      const uint32_t alpha = argb >> 24;
      const uint32_t red = ((argb >> 16) & 0xFFU) * alpha / 255U;
      const uint32_t green = ((argb >> 8) & 0xFFU) * alpha / 255U;
      const uint32_t blue = (argb & 0xFFU) * alpha / 255U;
      pixels[i] = (alpha << 24) | (red << 16) | (green << 8) | blue;
    }
    const D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                          D2D1_ALPHA_MODE_PREMULTIPLIED));
    const auto size = static_cast<UINT32>(px);
    const HRESULT hr = d2dContext_->CreateBitmap(
        D2D1::SizeU(size, size), pixels, size * 4U, &props,
        bitmap.GetAddressOf());
    if (FAILED(hr) != 0) {
      bitmap.Reset();
    }
    DeleteObject(dib);
  }
  return bitmap.Detach();
}

}  // namespace traymirror
