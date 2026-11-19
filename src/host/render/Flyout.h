// The "hidden icons" flyout (spec 6.4, rulings R1/R7): a second topmost
// window of the overlay thread, opened above the strip, showing a grid
// of the icons the strip does not. Created on demand and destroyed when
// closed — never hidden.
//
// The strip itself stays WS_EX_NOACTIVATE; this window does not, because
// a window whose thread is not foreground never receives the keyboard,
// and `Esc` has to close the flyout. It is opened by a user click on the
// chevron, which is what lets the process take the foreground.
#pragma once

#include "render/IconRasterizer.h"
#include "render/IconView.h"
#include "render/NativePopupSurface.h"
#include "render/PopupTransition.h"

#include <windows.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

namespace traymirror {

class Tooltip;

class Flyout {
 public:
  // `anchor` is the strip's screen rect: the grid opens above it. A
  // click on a cell calls `onClick`. `onClose` runs after the window is
  // gone and must not touch the Flyout (the owner destroys it there).
  // `wic` and `d2dDevice` are borrowed from the strip: the device avoids
  // creating a second graphics stack on the click path.
  static std::unique_ptr<Flyout> Create(HMONITOR monitor, const RECT& anchor,
                                        std::vector<IconView> icons,
                                        DispatchClick onClick,
                                        std::function<void()> onClose,
                                        IWICImagingFactory* wic = nullptr,
                                        ID2D1Device* d2dDevice = nullptr);
  ~Flyout();
  Flyout(const Flyout&) = delete;
  Flyout& operator=(const Flyout&) = delete;

  [[nodiscard]] HWND hwnd() const noexcept { return hwnd_; }
  // True when content is drawn into the composition surface rather than
  // with GDI; false means the composition stack is unavailable.
  [[nodiscard]] bool usesComposition() const noexcept;
  // True when the acrylic material transmits the background; false means
  // the opaque system-colour fallback is painted.
  [[nodiscard]] bool materialActive() const noexcept;
  // A close is in progress: the window is still displayed and fading but
  // no longer accepts input.
  [[nodiscard]] bool IsClosing() const noexcept;

  // Starts the close transition; with animations disabled (or an
  // invisible popup) it destroys immediately. `onClose` runs only when
  // the window is actually gone.
  void Close();
  // Reverses a close from the displayed opacity. False when the window
  // is already gone. Used by the chevron: clicking it during the close
  // animation must bring the same popup back, never race a new one.
  bool Reopen();
  // Destroys without a transition (the owner is going away).
  void CloseImmediately();
  void Update(std::vector<IconView> icons, const RECT& anchor);

 private:
  Flyout() = default;

  static bool EnsureClass();
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam,
                                  LPARAM lparam);
  LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

  void Paint(HDC dc);
  // Composition content: the same hover and glyphs as Paint(), drawn into
  // the material-backed surface. GDI cannot be used over the material.
  void DrawContentD2D(ID2D1DeviceContext* dc, int width, int height);
  // Returns the cached premultiplied bitmap for `view`, converting it on
  // first use. Null means "draw with DrawIconEx instead".
  [[nodiscard]] HBITMAP BitmapFor(const IconView& view);
  // The Direct2D counterpart of BitmapFor, created on the surface device.
  [[nodiscard]] ID2D1Bitmap* D2dBitmapFor(ID2D1DeviceContext* dc,
                                          const IconView& view);
  void PruneBitmaps();
  void ApplyTheme();
  // Applies one transition step: alpha to the layered window, destroy
  // when the fade reached zero, stop the timer when it settles.
  void ApplyStep(const PopupTransition::Step& step);
  void ApplyAlpha(int alpha);
  void TickTransition();
  void StartTimer();
  // Destroys the window now and runs the owner's callback exactly once.
  void DestroyNow();
  void SetHover(size_t index);
  [[nodiscard]] size_t HitTest(POINT pt) const;
  void DispatchAt(TrayEvent event, POINT pt);
  void TrackLeave(HWND hwnd);
  void ArmHover();
  // Shows the hovered cell's tip with the same window and measured profile
  // as the strip's tooltip; hides it when the cell has no text.
  void ShowTooltip();
  void HideTooltip();

  static constexpr size_t kNoHover = static_cast<size_t>(-1);

  struct CachedIcon {
    HICON icon = nullptr;
    UINT serial = 0;
    int px = 0;
    UniqueBitmap bitmap;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> d2d;
  };

  HWND hwnd_ = nullptr;
  HMONITOR monitor_ = nullptr;
  RECT anchor_ = {};
  bool dark_ = false;
  bool activated_ = false;  // an outside click closes only after this
  bool tracking_ = false;
  size_t hovered_ = kNoHover;
  int iconPx_ = 16;
  std::vector<IconView> icons_;
  std::vector<RECT> cells_;  // client-local, one per icon
  std::vector<CachedIcon> bitmaps_;
  std::unique_ptr<Tooltip> tooltip_;  // created on the first hover
  std::unique_ptr<NativePopupSurface> surface_;  // null: GDI fallback
  PopupTransition transition_;
  ULONGLONG tickMs_ = 0;  // last animation tick, for late-timer skipping
  bool timerRunning_ = false;
  IWICImagingFactory* wic_ = nullptr;  // owned by the overlay thread
  DispatchClick onClick_;
  std::function<void()> onClose_;
};

}  // namespace traymirror
