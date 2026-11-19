#include "render/Overlay.h"

#include "Log.h"
#include "render/CellLayout.h"
#include "render/IconRasterizer.h"
#include "render/Theme.h"
#include "win/Monitors.h"

#include <d2d1helper.h>
#include <windowsx.h>

#include <bit>
#include <algorithm>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <set>
#include <utility>

namespace traymirror {
namespace {

constexpr wchar_t kOverlayClass[] = L"TrayMirrorOverlay";

// Posted by an open flyout after it destroys itself: the reset happens
// outside the flyout's own message handler, so no object dies mid-frame.
constexpr UINT kMsgFlyoutClosed = WM_APP + 4;

bool SameTarget(const IconView& a, const IconView& b) {
  if (a.target.owner == nullptr || b.target.owner == nullptr) {
    return a.target.owner == b.target.owner && a.icon == b.icon && a.serial == b.serial;
  }
  return a.target.owner == b.target.owner && a.target.uid == b.target.uid &&
         a.target.callbackMessage == b.target.callbackMessage &&
         a.target.version == b.target.version && a.target.ownerPid == b.target.ownerPid;
}


}  // namespace

bool Overlay::EnsureClass() {
  static const bool registered = []() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;  // without it WM_LBUTTONDBLCLK never arrives
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
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
    case WM_SETCURSOR:
      SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));
      return TRUE;
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
    case WM_MOUSEHOVER:
      ShowHoveredTooltip();
      return 0;
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
      tracking_ = false;
      DispatchLeave();
      ClearHover();
      chevronHovered_ = false;
      dirty_ = true;
      Render();
      return 0;
    case WM_NCDESTROY:
      hwnd_ = nullptr;
      return DefWindowProcW(hwnd, message, wparam, lparam);
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case kMsgFlyoutClosed:
      if (flyout_ != nullptr && flyout_->hwnd() == nullptr) {
        flyout_.reset();  // ignore a stale close from an older flyout
      }
      return 0;
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
    case WM_DWMCOLORIZATIONCOLORCHANGED:
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
    std::function<void()> onGeometryInvalidated, DispatchClick onClick,
    TrayMetrics metrics, HWND taskbarOwner) {
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
  overlay->metrics_ = metrics;
  overlay->onGeometryInvalidated_ = std::move(onGeometryInvalidated);
  overlay->onClick_ = std::move(onClick);

  const HWND hwnd = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST |
          WS_EX_NOREDIRECTIONBITMAP, kOverlayClass,
      L"TrayMirrorOverlay", WS_POPUP, strip.left, strip.top, width, height,
      taskbarOwner, nullptr, GetModuleHandleW(nullptr), overlay.get());
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
    overlay->ReleaseComposition();
  }
  overlay->RefreshVisibility();
  overlay->Render();
  return overlay;
}

Overlay::~Overlay() {
  // The flyout borrows this strip's D2D device, so it must be gone before
  // the device is released; reset destroys its window without a fade.
  flyout_.reset();
  ReleaseComposition();
  if (hwnd_ != nullptr) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
}

bool Overlay::Reposition(const RECT& strip, TrayMetrics metrics) {
  const int width = strip.right - strip.left;
  const int height = strip.bottom - strip.top;
  if (width <= 0 || height <= 0) {
    return false;
  }
  if (width != strip_.right - strip_.left || height != strip_.bottom - strip_.top) {
    if (dcomp_ != nullptr && dcompVisual_ != nullptr && surface_ != nullptr) {
      Microsoft::WRL::ComPtr<IDCompositionSurface> replacement;
      if (FAILED(dcomp_->CreateSurface(static_cast<UINT>(width), static_cast<UINT>(height),
                                       DXGI_FORMAT_B8G8R8A8_UNORM,
                                       DXGI_ALPHA_MODE_PREMULTIPLIED, replacement.GetAddressOf())) ||
          FAILED(dcompVisual_->SetContent(replacement.Get())) || FAILED(dcomp_->Commit())) {
        dcompVisual_->SetContent(surface_.Get());
        dcomp_->Commit();
        return false;
      }
      surface_ = std::move(replacement);
    }
  }
  const bool moved = SetWindowPos(hwnd_, HWND_TOPMOST, strip.left, strip.top, width, height,
                                   SWP_NOACTIVATE) != FALSE;
  if (!moved) {
    return false;
  }
  strip_ = strip;
  metrics_ = metrics;
  dirty_ = true;
  geometrySafe_ = true;
  RefreshVisibility();
  if (moved && flyout_ != nullptr && flyout_->hwnd() != nullptr) {
    flyout_->Update(overflow_, FlyoutAnchor());
  }
  return moved;
}

void Overlay::SetGeometrySafe(bool safe) {
  geometrySafe_ = safe;
  RefreshVisibility();
}

void Overlay::RefreshVisibility() {
  const HWND owner = GetWindow(hwnd_, GW_OWNER);
  bool show = geometrySafe_ && surface_ != nullptr &&
              (owner == nullptr || IsWindowVisible(owner) != FALSE);
  const HWND foreground = GetForegroundWindow();
  RECT foregroundRect = {};
  MONITORINFO monitor = {};
  monitor.cbSize = sizeof(monitor);
  if (foreground != nullptr && foreground != owner &&
      GetWindowRect(foreground, &foregroundRect) != FALSE &&
      GetMonitorInfoW(monitor_, &monitor) != FALSE) {
    const HWND shell = GetShellWindow();
    bool desktop = foreground == shell || foreground == GetDesktopWindow();
    wchar_t name[64] = {};
    const HWND root = GetAncestor(foreground, GA_ROOT);
    GetClassNameW(root, name, static_cast<int>(std::size(name)));
    if (wcscmp(name, L"WorkerW") == 0 || wcscmp(name, L"Progman") == 0) {
      DWORD windowPid = 0;
      DWORD shellPid = 0;
      GetWindowThreadProcessId(root, &windowPid);
      GetWindowThreadProcessId(shell, &shellPid);
      desktop = desktop || (shellPid != 0 && windowPid == shellPid);
    }
    show = show && ShouldExposeTray(monitor.rcMonitor, foregroundRect, desktop);
  }
  if (show) {
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                   SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
  } else {
    if (tooltip_ != nullptr) {
      tooltip_->Hide();
    }
    CloseFlyout();
    ShowWindow(hwnd_, SW_HIDE);
  }
}

void Overlay::SetIcons(std::vector<IconView> icons,
                       std::vector<IconView> overflow) {
  const SIZE button = TrayButtonSizeForDpi(GetDpiForWindow(hwnd_), metrics_);
  const auto slots = static_cast<size_t>((strip_.right - strip_.left) / button.cx);
  const size_t reserved = overflow.empty() ? 0U : 1U;
  if (slots > 0 && icons.size() > slots - reserved) {
    const size_t keep = slots - 1;
    overflow.insert(overflow.begin(),
                    std::make_move_iterator(icons.begin() + static_cast<ptrdiff_t>(keep)),
                    std::make_move_iterator(icons.end()));
    icons.resize(keep);
  }
  const bool preserveHover = hovered_ < icons_.size() && hovered_ < icons.size() &&
      hasChevron_ == !overflow.empty() && SameTarget(icons_[hovered_], icons[hovered_]);
  if (!preserveHover) {
    DispatchLeave();
    hovered_ = kNoHover;
    if (tooltip_ != nullptr) {
      tooltip_->Hide();
    }
  }
  chevronHovered_ = chevronHovered_ && !overflow.empty();
  if (flyout_ != nullptr &&
      !std::ranges::equal(overflow_, overflow,
                   [](const IconView& a, const IconView& b) {
                     return SameTarget(a, b) && a.serial == b.serial &&
                            a.icon == b.icon && a.tip == b.tip;
                   })) {
    flyout_->Update(overflow, FlyoutAnchor());
  }
  icons_ = std::move(icons);
  overflow_ = std::move(overflow);
  dirty_ = true;
  if (preserveHover && tooltip_ != nullptr && IsWindowVisible(tooltip_->hwnd()) != FALSE) {
    ShowHoveredTooltip();
  }
}

void Overlay::ApplyTheme() { dark_ = SystemUsesDarkTheme(); }

bool Overlay::InitComposition() {
  auto log = InitHostLogging("");
  const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  ownsComApartment_ = SUCCEEDED(apartment);
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                              CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(wic_.GetAddressOf())))) {
    return false;
  }
  if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                 __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown**>(
                                     dwrite_.GetAddressOf())))) {
    return false;
  }
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
  cache_.Prune({});
  dwrite_.Reset();
  wic_.Reset();
  surface_.Reset();
  dcompVisual_.Reset();
  dcompTarget_.Reset();
  dcomp_.Reset();
  d2dContext_.Reset();
  d2dDevice_.Reset();
  d2dFactory_.Reset();
  d3d_.Reset();
  if (ownsComApartment_) {
    CoUninitialize();
    ownsComApartment_ = false;
  }
}

void Overlay::Render() {
  if (!dirty_) {
    return;
  }
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
  dc->SetDpi(96.0F, 96.0F);  // geometry is already in physical pixels
  // The strip paints nothing but its buttons: the taskbar underneath keeps
  // its own material and theme, which is what the real notification area
  // looks like (report 11).
  const D2D1_COLOR_F transparent = D2D1::ColorF(0.0F, 0.0F, 0.0F, 0.0F);
  dc->Clear(&transparent);
  DrawIcons(dc.Get(), width, height);
  hr = surface_->EndDraw();
  if (FAILED(hr) != 0) {
    log->warn("surface EndDraw failed ({:#x})", static_cast<unsigned>(hr));
    return;
  }
  hr = dcomp_->Commit();
  if (FAILED(hr) != 0) {
    log->warn("composition commit failed ({:#x})", static_cast<unsigned>(hr));
    return;
  }
  dirty_ = false;
}

std::optional<Overlay::Bitmap> Overlay::RenderToBitmap(int dpi, bool dark) {
  if (d2dContext_ == nullptr || dpi <= 0) {
    return std::nullopt;
  }
  const int width = strip_.right - strip_.left;
  const int height = strip_.bottom - strip_.top;
  if (width <= 0 || height <= 0) {
    return std::nullopt;
  }
  const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
      D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                        D2D1_ALPHA_MODE_PREMULTIPLIED),
      static_cast<float>(dpi), static_cast<float>(dpi));
  Microsoft::WRL::ComPtr<ID2D1Bitmap1> target;
  if (FAILED(d2dContext_->CreateBitmap(
          D2D1::SizeU(static_cast<UINT32>(width), static_cast<UINT32>(height)),
          nullptr, 0, props, target.GetAddressOf())) != 0) {
    return std::nullopt;
  }
  // A CPU-readable copy: Direct2D refuses to Map a render target, so the
  // pixels are staged into a bitmap created for CPU_READ.
  const D2D1_BITMAP_PROPERTIES1 readbackProps = D2D1::BitmapProperties1(
      D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                        D2D1_ALPHA_MODE_PREMULTIPLIED),
      static_cast<float>(dpi), static_cast<float>(dpi));
  Microsoft::WRL::ComPtr<ID2D1Bitmap1> readback;
  if (FAILED(d2dContext_->CreateBitmap(
          D2D1::SizeU(static_cast<UINT32>(width), static_cast<UINT32>(height)),
          nullptr, 0, readbackProps, readback.GetAddressOf())) != 0) {
    return std::nullopt;
  }
  Microsoft::WRL::ComPtr<ID2D1Image> previousTarget;
  d2dContext_->GetTarget(previousTarget.GetAddressOf());
  D2D1_MATRIX_3X2_F previousTransform = {};
  d2dContext_->GetTransform(&previousTransform);
  FLOAT previousDpiX = 96.0F;
  FLOAT previousDpiY = 96.0F;
  d2dContext_->GetDpi(&previousDpiX, &previousDpiY);
  const bool previousDark = dark_;

  d2dContext_->SetTarget(target.Get());
  d2dContext_->SetTransform(D2D1::Matrix3x2F::Identity());
  d2dContext_->SetDpi(96.0F, 96.0F);
  dark_ = dark;
  const D2D1_COLOR_F transparent = D2D1::ColorF(0.0F, 0.0F, 0.0F, 0.0F);
  d2dContext_->BeginDraw();
  d2dContext_->Clear(&transparent);
  DrawIcons(d2dContext_.Get(), width, height, dpi);
  // EndDraw is what flushes the batch: without it the target bitmap is
  // still empty when it is copied.
  const HRESULT drawn = d2dContext_->EndDraw();

  dark_ = previousDark;
  d2dContext_->SetDpi(previousDpiX, previousDpiY);
  d2dContext_->SetTransform(previousTransform);
  d2dContext_->SetTarget(previousTarget.Get());

  if (FAILED(drawn) != 0) {
    return std::nullopt;
  }
  D2D1_MAPPED_RECT mapped = {};
  if (FAILED(readback->CopyFromBitmap(nullptr, target.Get(), nullptr)) != 0 ||
      FAILED(readback->Map(D2D1_MAP_OPTIONS_READ, &mapped)) != 0) {
    return std::nullopt;
  }
  Bitmap out;
  out.width = width;
  out.height = height;
  out.pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height));
  for (int y = 0; y < height; ++y) {
    std::memcpy(&out.pixels[static_cast<size_t>(y) * static_cast<size_t>(width)],
                mapped.bits + (static_cast<size_t>(y) * mapped.pitch),
                static_cast<size_t>(width) * sizeof(uint32_t));
  }
  readback->Unmap();
  return out;
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
    const bool changed = chevronHovered_ != onChevron;
    if (changed) {
      chevronHovered_ = onChevron;
      dirty_ = true;
      Render();
    }
    if (hovered_ != kNoHover) {
      DispatchLeave();  // the pointer left the icon for the gap
      ClearHover();
    }
    if (onChevron && changed) {
      ArmHover();
    } else if (!onChevron && tooltip_ != nullptr) {
      tooltip_->Hide();
    }
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
  if (tooltip_ != nullptr) {
    tooltip_->Hide();
  }
  ArmHover();
  dirty_ = true;
  Render();
  if (index >= icons_.size()) {
    return;
  }
  // The pointer entered this cell: v4 owners get NIN_POPUPOPEN (report 07).
  DispatchGesture(TrayEvent::kMouseEnter, pt);
}

void Overlay::ShowHoveredTooltip() {
  const bool chevron = chevronHovered_ && hasChevron_;
  if (!chevron && (hovered_ >= icons_.size() || hovered_ >= cells_.size())) {
    return;
  }
  if (tooltip_ == nullptr) {
    tooltip_ = Tooltip::Create(hwnd_);
  }
  const std::wstring text = chevron ? L"Show hidden icons" : icons_[hovered_].tip;
  if (tooltip_ != nullptr && !text.empty()) {
    // The tooltip places itself above the icon's screen rect (spec 6.4).
    const RECT& cell = chevron ? chevronCell_ : cells_[hovered_];
    const RECT iconRect = {.left = strip_.left + cell.left,
                           .top = strip_.top,
                           .right = strip_.left + cell.right,
                           .bottom = strip_.bottom};
    tooltip_->Show(text, iconRect);
  } else if (tooltip_ != nullptr) {
    tooltip_->Hide();
  }
}

void Overlay::ArmHover() {
  TRACKMOUSEEVENT track = {};
  track.cbSize = sizeof(track);
  track.dwFlags = TME_HOVER;
  track.hwndTrack = hwnd_;
  track.dwHoverTime = HOVER_DEFAULT;
  TrackMouseEvent(&track);
}

RECT Overlay::FlyoutAnchor() const {
  const SIZE button = TrayButtonSizeForDpi(GetDpiForWindow(hwnd_), metrics_);
  return RECT{.left = strip_.left, .top = strip_.top,
               .right = (std::min)(strip_.right, strip_.left + button.cx),
               .bottom = strip_.bottom};
}

bool Overlay::HandleChevronClick(POINT pt) {
  if (!hasChevron_ || PtInRect(&chevronCell_, pt) == 0) {
    return false;
  }
  ToggleFlyout();
  return true;
}

void Overlay::ToggleFlyout() {
  if (flyout_ != nullptr && flyout_->hwnd() != nullptr) {
    if (flyout_->IsClosing()) {
      flyout_->Reopen();  // the chevron reverses the fade, not a new popup
    } else {
      flyout_->Close();
    }
    return;
  }
  flyout_.reset();
  if (overflow_.empty() || hwnd_ == nullptr) {
    return;
  }
  // The owner is told after the window is gone; the reset below happens
  // on the message loop, never inside the flyout's own handler.
  flyout_ = Flyout::Create(monitor_, FlyoutAnchor(), overflow_, onClick_, [this]() {
    if (hwnd_ != nullptr) {
      PostMessageW(hwnd_, kMsgFlyoutClosed, 0, 0);
    }
  }, wic_.Get(), d2dDevice_.Get());
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

void Overlay::DrawIcons(ID2D1DeviceContext* dc, int width, int height,
                        int dpiOverride) {
  int dpi = dpiOverride;
  if (dpi <= 0) {
    dpi = static_cast<int>(GetDpiForWindow(hwnd_));
  }
  if (dpi <= 0) {
    dpi = 96;
  }
  const int iconPx = IconPxForDpi(static_cast<UINT>(dpi));
  const int pad = Px(metrics::kIconPad, dpi);
  const StripLayout layout =
      LayoutStrip(width, height, iconPx, pad, icons_.size(), !overflow_.empty(),
                  TrayButtonSizeForDpi(static_cast<UINT>(dpi), metrics_));
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
    // The cell is the whole tray button; the glyph is centred in it.
    const int glyphLeft = rect.left + ((rect.right - rect.left - iconPx) / 2);
    const int glyphTop = rect.top + ((rect.bottom - rect.top - iconPx) / 2);
    const D2D1_RECT_F glyph = D2D1::RectF(
        static_cast<float>(glyphLeft), static_cast<float>(glyphTop),
        static_cast<float>(glyphLeft + iconPx), static_cast<float>(glyphTop + iconPx));
    ID2D1Bitmap* bitmap = cache_.Get(icon.icon, icon.serial, iconPx);
    live.insert(
        IconCacheKey{.icon = icon.icon, .serial = icon.serial, .px = iconPx});
    if (i == hovered_) {
      DrawHighlight(dc, cell, dpi);
    }
    if (bitmap != nullptr) {
      dc->DrawBitmap(bitmap, glyph, 1.0F,
                     D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else {
      DrawPlaceholder(dc, glyph);
    }
  }
  if (hasChevron_) {
    const D2D1_RECT_F cell = D2D1::RectF(
        static_cast<float>(chevronCell_.left),
        static_cast<float>(chevronCell_.top),
        static_cast<float>(chevronCell_.right),
        static_cast<float>(chevronCell_.bottom));
    if (chevronHovered_) {
      DrawHighlight(dc, cell, dpi);
    }
    DrawChevron(dc, cell);
  }
  cache_.Prune(live);
}

void Overlay::DrawHighlight(ID2D1DeviceContext* dc,
                            const D2D1_RECT_F& cell, int dpi) const {
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
  const ThemeTokens tokens = ThemeTokens::Current(dark_);
  if (FAILED(dc->CreateSolidColorBrush(tokens.hover, brush.GetAddressOf())) != 0 ||
      brush == nullptr) {
    return;
  }
  const auto radius = static_cast<float>(Px(metrics::kControlCornerRadius, dpi));
  const RECT button = {.left = static_cast<LONG>(cell.left),
                       .top = static_cast<LONG>(cell.top),
                       .right = static_cast<LONG>(cell.right),
                       .bottom = static_cast<LONG>(cell.bottom)};
  const RECT visual = HighlightBounds(button, dpi);
  const D2D1_RECT_F inset = D2D1::RectF(static_cast<float>(visual.left),
                                       static_cast<float>(visual.top),
                                       static_cast<float>(visual.right),
                                       static_cast<float>(visual.bottom));
  dc->FillRoundedRectangle(D2D1::RoundedRect(inset, radius, radius), brush.Get());
}

void Overlay::DrawChevron(ID2D1DeviceContext* dc,
                          const D2D1_RECT_F& cell) const {
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
  const D2D1_COLOR_F color = ThemeTokens::Current(dark_).text;
  if (FAILED(dc->CreateSolidColorBrush(color, brush.GetAddressOf())) != 0 ||
      brush == nullptr) {
    return;
  }
  if (dwrite_ == nullptr) {
    return;
  }
  Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
  const float size = (cell.right - cell.left) * 0.375F;
  if (FAILED(dwrite_->CreateTextFormat(
          L"Segoe Fluent Icons", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
          DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"",
          format.GetAddressOf()))) {
    return;
  }
  format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
  format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
  Microsoft::WRL::ComPtr<IDWriteTextLayout> text;
  if (SUCCEEDED(dwrite_->CreateTextLayout(
          L"\uE70E", 1, format.Get(), cell.right - cell.left,
          cell.bottom - cell.top, text.GetAddressOf()))) {
    dc->DrawTextLayout(D2D1::Point2F(cell.left, cell.top), text.Get(), brush.Get());
  }
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
  // One conversion for the strip and the flyout (plan task 4). It
  // premultiplies exactly once and does not resample an already-correct
  // source; both matter for glyphs whose strokes land on half pixels.
  if (d2dContext_ == nullptr || wic_ == nullptr || icon == nullptr || px <= 0) {
    return nullptr;
  }
  RasterIcon raster;
  if (!RasterizeIcon(wic_.Get(), icon, px, &raster)) {
    return nullptr;
  }
  const auto stride =
      static_cast<UINT>(raster.width) * static_cast<UINT>(sizeof(uint32_t));
  const D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                        D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0F, 96.0F);
  Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
  if (FAILED(d2dContext_->CreateBitmap(
          D2D1::SizeU(static_cast<UINT>(raster.width),
                      static_cast<UINT>(raster.height)),
          raster.pixels.data(), stride, &props, bitmap.GetAddressOf()))) {
    return nullptr;
  }
  return bitmap.Detach();
}

}  // namespace traymirror
