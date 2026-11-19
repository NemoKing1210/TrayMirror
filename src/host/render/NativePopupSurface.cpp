#include "render/NativePopupSurface.h"

#include "Log.h"
#include "render/Theme.h"

#include <d2d1helper.h>
#include <dwmapi.h>

#include <algorithm>
#include <iterator>

namespace traymirror {
namespace {

// WCA_ACCENT_POLICY with ACCENT_ENABLE_ACRYLICBLURBEHIND. This is the
// only Windows path measured to transmit the desktop behind a popup on
// this build (see the header); DWM system backdrops do not.
constexpr int kWcaAccentPolicy = 19;
constexpr int kAccentDisabled = 0;
constexpr int kAccentEnabledAcrylic = 4;

struct AccentPolicy {
  int state;
  int flags;
  int gradientColor;  // ABGR
  int animationId;
};

struct WindowCompositionAttributeData {
  int attribute;
  void* data;
  unsigned long sizeOfData;
};

using SetWindowCompositionAttributeFn =
    BOOL(WINAPI*)(HWND, WindowCompositionAttributeData*);

SetWindowCompositionAttributeFn LoadAccentSetter() {
  const HMODULE user32 = GetModuleHandleW(L"user32.dll");
  if (user32 == nullptr) {
    return nullptr;
  }
  return reinterpret_cast<SetWindowCompositionAttributeFn>(
      GetProcAddress(user32, "SetWindowCompositionAttribute"));
}

bool ApplyAccent(HWND hwnd, int state, uint32_t tint) {
  const SetWindowCompositionAttributeFn setAccent = LoadAccentSetter();
  if (setAccent == nullptr) {
    return false;
  }
  AccentPolicy policy = {.state = state,
                         .flags = 0,
                         .gradientColor = static_cast<int>(tint),
                         .animationId = 0};
  WindowCompositionAttributeData data = {.attribute = kWcaAccentPolicy,
                                         .data = &policy,
                                         .sizeOfData = sizeof(policy)};
  return setAccent(hwnd, &data) != FALSE;
}

D2D1_COLOR_F D2DColor(COLORREF color, float alpha = 1.0F) {
  return D2D1::ColorF(GetRValue(color) / 255.0F, GetGValue(color) / 255.0F,
                      GetBValue(color) / 255.0F, alpha);
}

}  // namespace

PopupMaterial ResolvePopupMaterial(const PopupEnvironment& env) {
  if (env.transparencyEnabled && !env.highContrast && env.acrylicAvailable &&
      env.compositionAvailable) {
    return PopupMaterial::kAcrylic;
  }
  return PopupMaterial::kOpaque;
}

uint32_t PopupAcrylicTint(bool dark) {
  return dark ? 0xCC202020U : 0xCCF3F3F3U;
}

bool SystemTransparencyEnabled() {
  DWORD enabled = 1;
  DWORD size = sizeof(enabled);
  if (RegGetValueW(HKEY_CURRENT_USER,
                   L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\"
                   L"Personalize",
                   L"EnableTransparency", RRF_RT_REG_DWORD, nullptr, &enabled,
                   &size) == ERROR_SUCCESS) {
    return enabled != 0;
  }
  return true;  // Windows enables transparency effects by default
}

std::unique_ptr<NativePopupSurface> NativePopupSurface::Attach(
    HWND hwnd, ID2D1Device* sharedDevice) {
  if (hwnd == nullptr || IsWindow(hwnd) == FALSE) {
    return nullptr;
  }
  std::unique_ptr<NativePopupSurface> surface(new NativePopupSurface());
  surface->hwnd_ = hwnd;
  surface->d2dDevice_ = sharedDevice;  // retains the caller's device
  if (!surface->Init()) {
    return nullptr;
  }
  return surface;
}

NativePopupSurface::~NativePopupSurface() {
  surface_.Reset();
  visual_.Reset();
  target_.Reset();
  dcomp_.Reset();
  d2dDevice_.Reset();
  d2dFactory_.Reset();
  d3d_.Reset();
  if (ownsComApartment_) {
    CoUninitialize();
  }
}

bool NativePopupSurface::Init() {
  auto log = InitHostLogging("");
  const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(apartment) && apartment != RPC_E_CHANGED_MODE) {
    log->warn("popup surface COM init failed ({:#x})",
              static_cast<unsigned>(apartment));
    return false;
  }
  ownsComApartment_ = SUCCEEDED(apartment);

  if (d2dDevice_ == nullptr) {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0,
                                        D3D_FEATURE_LEVEL_10_1,
                                        D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                   static_cast<UINT>(std::size(levels)),
                                   D3D11_SDK_VERSION, d3d_.GetAddressOf(),
                                   nullptr, nullptr);
    if (FAILED(hr)) {
      hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                             D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                             static_cast<UINT>(std::size(levels)),
                             D3D11_SDK_VERSION, d3d_.GetAddressOf(), nullptr,
                             nullptr);
    }
    if (FAILED(hr)) {
      log->warn("popup surface D3D11 device failed ({:#x})",
                static_cast<unsigned>(hr));
      return false;
    }

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    hr = d3d_.As(&dxgiDevice);
    if (FAILED(hr)) {
      return false;
    }
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                           d2dFactory_.GetAddressOf());
    if (FAILED(hr)) {
      return false;
    }
    hr = d2dFactory_->CreateDevice(dxgiDevice.Get(),
                                   d2dDevice_.GetAddressOf());
    if (FAILED(hr)) {
      return false;
    }
  }
  // The D2D device is the rendering device, which is what lets BeginDraw
  // hand back an ID2D1DeviceContext rather than the v1 surface.
  HRESULT hr = DCompositionCreateDevice2(d2dDevice_.Get(),
                                         IID_PPV_ARGS(dcomp_.GetAddressOf()));
  if (FAILED(hr)) {
    log->warn("popup surface DCompositionCreateDevice2 failed ({:#x})",
              static_cast<unsigned>(hr));
    return false;
  }
  hr = dcomp_->CreateTargetForHwnd(hwnd_, TRUE, target_.GetAddressOf());
  if (FAILED(hr)) {
    return false;
  }
  hr = dcomp_->CreateVisual(visual_.GetAddressOf());
  if (FAILED(hr)) {
    return false;
  }
  hr = target_->SetRoot(visual_.Get());
  if (FAILED(hr)) {
    return false;
  }

  RECT client = {};
  if (GetClientRect(hwnd_, &client) == FALSE ||
      !Resize(static_cast<int>(client.right - client.left),
              static_cast<int>(client.bottom - client.top))) {
    return false;
  }
  environment_.compositionAvailable = true;
  return true;
}

bool NativePopupSurface::Resize(int width, int height) {
  if (dcomp_ == nullptr || visual_ == nullptr || width <= 0 || height <= 0) {
    return false;
  }
  if (surface_ != nullptr && width == width_ && height == height_) {
    return true;
  }
  Microsoft::WRL::ComPtr<IDCompositionSurface> surface;
  HRESULT hr = dcomp_->CreateSurface(static_cast<UINT>(width),
                                     static_cast<UINT>(height),
                                     DXGI_FORMAT_B8G8R8A8_UNORM,
                                     DXGI_ALPHA_MODE_PREMULTIPLIED,
                                     surface.GetAddressOf());
  if (FAILED(hr)) {
    return false;
  }
  hr = visual_->SetContent(surface.Get());
  if (FAILED(hr)) {
    return false;
  }
  hr = dcomp_->Commit();
  if (FAILED(hr)) {
    return false;
  }
  surface_ = surface;
  width_ = width;
  height_ = height;
  return true;
}

bool NativePopupSurface::ApplyTheme(bool dark) {
  if (hwnd_ == nullptr) {
    return false;
  }
  dark_ = dark;
  const BOOL immersive = dark ? TRUE : FALSE;
  DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &immersive,
                        sizeof(immersive));
  const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
  DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner,
                        sizeof(corner));

  environment_.transparencyEnabled = SystemTransparencyEnabled();
  environment_.highContrast = SystemUsesHighContrast();
  const bool allowed =
      environment_.transparencyEnabled && !environment_.highContrast;
  acrylicApplied_ = allowed && ApplyAccent(hwnd_, kAccentEnabledAcrylic,
                                           PopupAcrylicTint(dark));
  environment_.acrylicAvailable = acrylicApplied_;
  if (!allowed) {
    // A theme switch into high contrast or with transparency off must
    // remove an accent that was applied earlier, not leave a stale one.
    ApplyAccent(hwnd_, kAccentDisabled, 0);
  }
  return materialActive();
}

bool NativePopupSurface::Render(const DrawContent& draw) {
  if (surface_ == nullptr || dcomp_ == nullptr || width_ <= 0 || height_ <= 0) {
    return false;
  }
  RECT update = {.left = 0, .top = 0, .right = width_, .bottom = height_};
  POINT offset = {};
  Microsoft::WRL::ComPtr<ID2D1DeviceContext> dc;
  HRESULT hr = surface_->BeginDraw(&update, IID_PPV_ARGS(dc.GetAddressOf()),
                                   &offset);
  if (FAILED(hr) || dc == nullptr) {
    return false;
  }
  dc->SetTransform(D2D1::Matrix3x2F::Translation(
      static_cast<float>(offset.x), static_cast<float>(offset.y)));
  dc->SetDpi(96.0F, 96.0F);  // geometry is already in physical pixels
  // Over acrylic the clear must stay transparent: the window material is
  // the background. The opaque fallback is the theme's system surface, so
  // the popup is still legible when the OS disables the effect.
  const D2D1_COLOR_F background = materialActive()
      ? D2D1::ColorF(0.0F, 0.0F, 0.0F, 0.0F)
      : D2DColor(ThemeTokens::Current(dark_).surface);
  dc->Clear(&background);
  if (draw) {
    draw(dc.Get(), width_, height_);
  }
  hr = surface_->EndDraw();
  if (FAILED(hr)) {
    return false;
  }
  return SUCCEEDED(dcomp_->Commit());
}

}  // namespace traymirror
