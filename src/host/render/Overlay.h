// One overlay strip per secondary taskbar (spec 6.1/6.2): a topmost,
// never-activated borderless window over the taskbar row, drawn with
// Direct2D into a DirectComposition surface. Everything is created on
// the overlay thread; the model stays off this type.
#pragma once

#include "dispatch/Dispatch.h"
#include "render/Flyout.h"
#include "render/IconCache.h"
#include "render/IconView.h"
#include "render/Tooltip.h"
#include "win/Monitors.h"

#include <windows.h>

#include <d2d1_1.h>
#include <dcomp.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <wincodec.h>
#include <dwrite.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace traymirror {

// Strip-local hit test over the drawn cells; true and the cell index on
// a hit (the gap between cells is not a hit).
bool HitTestIcon(const std::vector<RECT>& cells, POINT pt, size_t* index);

class Overlay {
 public:
  // `onGeometryInvalidated` runs on the overlay thread when the window
  // must be rebuilt (DPI change); it must only post, never rebuild here.
  // `onClick` receives gestures on the drawn cells, in screen coordinates.
  static std::unique_ptr<Overlay> Create(
      HMONITOR monitor, const RECT& strip,
      std::function<void()> onGeometryInvalidated = {},
      DispatchClick onClick = {}, TrayMetrics metrics = {}, HWND taskbarOwner = nullptr);
  ~Overlay();
  Overlay(const Overlay&) = delete;
  Overlay& operator=(const Overlay&) = delete;

  [[nodiscard]] HWND hwnd() const noexcept { return hwnd_; }
  [[nodiscard]] HMONITOR monitor() const noexcept { return monitor_; }
  bool Reposition(const RECT& strip, TrayMetrics metrics);
  void SetGeometrySafe(bool safe);
  void RefreshVisibility();
  // Replaces the icon list; marks the strip dirty. HICONs are borrowed.
  // `overflow` are the icons the strip does not show: when it is not
  // empty the strip draws a chevron that opens the flyout (spec 6.4).
  void SetIcons(std::vector<IconView> icons,
                std::vector<IconView> overflow = {});
  // Redraws only when dirty (spec 6.2: no timer, no polling).
  void Render();

  // A read-back of what the strip draws, in 0xAARRGGBB, row-major. This
  // is the test hook for the visual spec (spec 6.3): the same drawing
  // path, rendered into an offscreen target at `dpi` instead of the
  // composition surface. Empty when composition is unavailable.
  struct Bitmap {
    int width = 0;
    int height = 0;
    std::vector<uint32_t> pixels;
    [[nodiscard]] uint32_t At(int x, int y) const {
      return pixels[(static_cast<size_t>(y) * static_cast<size_t>(width)) +
                    static_cast<size_t>(x)];
    }
  };
  [[nodiscard]] std::optional<Bitmap> RenderToBitmap(int dpi, bool dark);

 private:
  Overlay() = default;

  static bool EnsureClass();
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam,
                                  LPARAM lparam);
  LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

  bool InitComposition();
  void ReleaseComposition();
  void ApplyTheme();
  // `dpi` of 0 means the window's DPI; the bitmap path passes one.
  void DrawIcons(ID2D1DeviceContext* dc, int width, int height, int dpi = 0);
  [[nodiscard]] ID2D1Bitmap* IconBitmap(HICON icon, int px) const;
  static void DrawPlaceholder(ID2D1DeviceContext* dc, const D2D1_RECT_F& cell);
  void DrawHighlight(ID2D1DeviceContext* dc,
                     const D2D1_RECT_F& cell, int dpi) const;
  void DrawChevron(ID2D1DeviceContext* dc, const D2D1_RECT_F& cell) const;
  void HandleMouseMove(HWND hwnd, POINT pt);
  void ShowHoveredTooltip();
  void ArmHover();
  [[nodiscard]] RECT FlyoutAnchor() const;
  // True when the click landed on the chevron and was consumed.
  bool HandleChevronClick(POINT pt);
  void ToggleFlyout();
  void CloseFlyout();
  void DispatchGesture(TrayEvent event, POINT pt);
  void DispatchLeave();
  void ClearHover();

  static constexpr size_t kNoHover = static_cast<size_t>(-1);

  HWND hwnd_ = nullptr;
  HMONITOR monitor_ = nullptr;
  RECT strip_ = {};
  TrayMetrics metrics_;
  bool ownsComApartment_ = false;
  bool dirty_ = true;
  bool geometrySafe_ = true;
  bool dark_ = false;
  bool tracking_ = false;
  size_t hovered_ = kNoHover;
  bool chevronHovered_ = false;
  bool hasChevron_ = false;
  RECT chevronCell_ = {};
  std::unique_ptr<Tooltip> tooltip_;
  // Open only while the user has it up; closed on Esc, outside click, a
  // model change or a click on one of its cells (R7: destroyed, not hidden).
  std::unique_ptr<Flyout> flyout_;
  std::vector<IconView> icons_;
  std::vector<IconView> overflow_;
  std::vector<RECT> cells_;  // strip-local, one per drawn icon (hit-testing)
  std::function<void()> onGeometryInvalidated_;
  DispatchClick onClick_;
  IconCache cache_{[this](HICON icon, int px) { return IconBitmap(icon, px); }};

  Microsoft::WRL::ComPtr<ID3D11Device> d3d_;
  Microsoft::WRL::ComPtr<IWICImagingFactory> wic_;
  Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_;
  Microsoft::WRL::ComPtr<ID2D1Factory1> d2dFactory_;
  Microsoft::WRL::ComPtr<ID2D1Device> d2dDevice_;
  Microsoft::WRL::ComPtr<ID2D1DeviceContext> d2dContext_;
  // Created via DCompositionCreateDevice2 with the D2D device: only such
  // a device lets BeginDraw hand back an ID2D1DeviceContext (docs are
  // explicit that the v1 device yields E_INVALIDARG).
  Microsoft::WRL::ComPtr<IDCompositionDesktopDevice> dcomp_;
  Microsoft::WRL::ComPtr<IDCompositionTarget> dcompTarget_;
  Microsoft::WRL::ComPtr<IDCompositionVisual2> dcompVisual_;
  Microsoft::WRL::ComPtr<IDCompositionSurface> surface_;
};

}  // namespace traymirror
