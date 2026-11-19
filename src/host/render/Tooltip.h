// Tooltip for one overlay strip (spec 6.4, plan ruling R3): the system
// tooltips_class32 in tracking mode, positioned by the overlay. A custom
// Win11-style window is deferred to the visual pass.
#pragma once

#include <windows.h>

#include <commctrl.h>

#include <memory>
#include <string>

namespace traymirror {

class Tooltip {
 public:
  static std::unique_ptr<Tooltip> Create(HWND owner);
  ~Tooltip();
  Tooltip(const Tooltip&) = delete;
  Tooltip& operator=(const Tooltip&) = delete;

  // `anchor` is a screen point (TTF_ABSOLUTE): the tooltip's top-left.
  void Show(const std::wstring& text, POINT anchor);
  void Hide();
  [[nodiscard]] HWND hwnd() const noexcept { return hwnd_; }

 private:
  Tooltip() = default;

  HWND hwnd_ = nullptr;
  TOOLINFOW tool_ = {};
  std::wstring text_;
  bool registered_ = false;
};

}  // namespace traymirror
