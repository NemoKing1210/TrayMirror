#include "render/Overlay.h"

#include "Log.h"

#include <d2d1helper.h>

#include <bit>
#include <iterator>
#include <utility>

namespace traymirror {
namespace {

constexpr wchar_t kOverlayClass[] = L"TrayMirrorOverlay";

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
    std::function<void()> onGeometryInvalidated) {
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
  ReleaseComposition();
  if (hwnd_ != nullptr) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
}

void Overlay::SetIcons(std::vector<IconView> icons) {
  icons_ = std::move(icons);
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
  POINT offset = {};
  Microsoft::WRL::ComPtr<ID2D1DeviceContext> dc;
  RECT update = {};
  update.right = strip_.right - strip_.left;
  update.bottom = strip_.bottom - strip_.top;
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

}  // namespace traymirror
