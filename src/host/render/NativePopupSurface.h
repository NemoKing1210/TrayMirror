// Popup material and composition content for the overflow flyout.
//
// Measured 2026-11-19 (`.superpowers/proto/popup-material/`): on this
// Windows 11 build DWM system backdrops (DWMSBT_MAINWINDOW/TRANSIENTWINDOW)
// produce a solid wallpaper-derived fill and transmit nothing
// (delta 0 between contrasting backgrounds); SetWindowCompositionAttribute
// acrylic transmits and blurs what is behind the window (delta 32/255,
// blur ratio 0.03). GDI content over that material is additive and
// black-keyed, so content must be composition content: this surface hands
// the caller a Direct2D context backed by an IDCompositionSurface.
//
// The caller must not depend on the backend: it asks for material state,
// renders through the DrawContent callback and falls back to its own
// opaque drawing when composition is unavailable.
#pragma once

#include <windows.h>

#include <d2d1_1.h>
#include <dcomp.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>
#include <memory>

namespace traymirror {

enum class PopupMaterial : std::uint8_t {
  kAcrylic,  // the window material transmits the background
  kOpaque,   // the surface paints an opaque system-colour fill
};

struct PopupEnvironment {
  bool transparencyEnabled = false;  // personalisation "Transparency"
  bool highContrast = false;
  bool acrylicAvailable = false;     // SetWindowCompositionAttribute worked
  bool compositionAvailable = false; // D3D + D2D + DirectComposition ready
};

// Acrylic only when the OS allows it and both backends came up; any
// missing piece is the intentional opaque fallback, never a half-applied
// effect.
[[nodiscard]] PopupMaterial ResolvePopupMaterial(const PopupEnvironment& env);

// ACCENT_POLICY.gradientColor is ABGR. The measured Win11 tint is the
// theme's SolidBackgroundFillColorBase at 0xCC opacity.
[[nodiscard]] uint32_t PopupAcrylicTint(bool dark);

// Personalisation > Colours > Transparency effects. Missing means on.
bool SystemTransparencyEnabled();

class NativePopupSurface {
 public:
  // Draws popup content into a premultiplied composition surface. The
  // context is already cleared (transparent over acrylic, opaque system
  // colour otherwise), has its origin at the surface's top-left and
  // reports 96 DPI; `width`/`height` are physical pixels.
  using DrawContent =
      std::function<void(ID2D1DeviceContext* dc, int width, int height)>;

  // Null when the window is invalid or the composition stack cannot be
  // created; the caller then paints with GDI, which is opaque but correct.
  // `sharedDevice` reuses the strip's D3D/D2D device instead of paying
  // device creation on the open path; the caller must outlive the surface.
  static std::unique_ptr<NativePopupSurface> Attach(
      HWND hwnd, ID2D1Device* sharedDevice = nullptr);
  ~NativePopupSurface();
  NativePopupSurface(const NativePopupSurface&) = delete;
  NativePopupSurface& operator=(const NativePopupSurface&) = delete;

  // Recreates the composition surface at the new client size. False
  // leaves the previous surface in place (nothing is torn down).
  bool Resize(int width, int height);
  // Applies the dark/light tint and DWM corner, re-resolves the material
  // and returns whether the acrylic material is active afterwards.
  bool ApplyTheme(bool dark);
  // Begins a draw, clears to the material-appropriate background and runs
  // `draw`. False when the surface is gone; the caller's fallback is GDI.
  bool Render(const DrawContent& draw);

  [[nodiscard]] bool compositionAvailable() const noexcept {
    return environment_.compositionAvailable;
  }
  [[nodiscard]] bool materialActive() const noexcept {
    return ResolvePopupMaterial(environment_) == PopupMaterial::kAcrylic;
  }
  [[nodiscard]] PopupMaterial material() const noexcept {
    return ResolvePopupMaterial(environment_);
  }
  [[nodiscard]] const PopupEnvironment& environment() const noexcept {
    return environment_;
  }

 private:
  NativePopupSurface() = default;
  bool Init();
  bool ApplyAcrylic(uint32_t tint);

  HWND hwnd_ = nullptr;
  bool ownsComApartment_ = false;
  bool dark_ = false;
  bool acrylicApplied_ = false;
  int width_ = 0;
  int height_ = 0;
  PopupEnvironment environment_;

  Microsoft::WRL::ComPtr<ID3D11Device> d3d_;
  Microsoft::WRL::ComPtr<ID2D1Factory1> d2dFactory_;
  Microsoft::WRL::ComPtr<ID2D1Device> d2dDevice_;
  Microsoft::WRL::ComPtr<IDCompositionDesktopDevice> dcomp_;
  Microsoft::WRL::ComPtr<IDCompositionTarget> target_;
  Microsoft::WRL::ComPtr<IDCompositionVisual2> visual_;
  Microsoft::WRL::ComPtr<IDCompositionSurface> surface_;
};

}  // namespace traymirror
