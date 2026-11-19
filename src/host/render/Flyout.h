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

#include "render/IconView.h"

#include <windows.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

namespace traymirror {

class Flyout {
 public:
  // `anchor` is the strip's screen rect: the grid opens above it. A
  // click on a cell calls `onClick`. `onClose` runs after the window is
  // gone and must not touch the Flyout (the owner destroys it there).
  static std::unique_ptr<Flyout> Create(HMONITOR monitor, const RECT& anchor,
                                        std::vector<IconView> icons,
                                        DispatchClick onClick,
                                        std::function<void()> onClose);
  ~Flyout();
  Flyout(const Flyout&) = delete;
  Flyout& operator=(const Flyout&) = delete;

  [[nodiscard]] HWND hwnd() const noexcept { return hwnd_; }

  // Destroys the window; `onClose` runs once. Calling it again is a
  // no-op. The owner may call this at any time (a model change under an
  // open flyout has to close it).
  void Close();

 private:
  Flyout() = default;

  static bool EnsureClass();
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam,
                                  LPARAM lparam);
  LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

  void Paint(HDC dc);
  void SetHover(size_t index);
  [[nodiscard]] size_t HitTest(POINT pt) const;
  void DispatchAt(TrayEvent event, POINT pt);
  void TrackLeave(HWND hwnd);

  static constexpr size_t kNoHover = static_cast<size_t>(-1);

  HWND hwnd_ = nullptr;
  bool dark_ = false;
  bool activated_ = false;  // an outside click closes only after this
  bool tracking_ = false;
  size_t hovered_ = kNoHover;
  int iconPx_ = 16;
  std::vector<IconView> icons_;
  std::vector<RECT> cells_;  // client-local, one per icon
  DispatchClick onClick_;
  std::function<void()> onClose_;
};

}  // namespace traymirror
