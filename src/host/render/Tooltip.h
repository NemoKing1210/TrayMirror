// The measured Win11 tray tooltip (spec 6.4, report 09): a topmost
// no-activate window that paints the solid measured surface. Text is
// drawn with GDI and the system status font because that is what the
// native tooltip uses, so the measured padding/advance values and the
// font-linking behaviour are reproduced exactly instead of through a
// second shaping stack.
#pragma once

#include <windows.h>

#include <functional>
#include <memory>
#include <string>

namespace traymirror {

class Tooltip {
 public:
  // `dark` resolves the current theme; injectable so a test never reads
  // the user's personalization setting.
  using ThemeProbe = std::function<bool()>;

  static std::unique_ptr<Tooltip> Create(HWND owner, ThemeProbe dark = {});
  ~Tooltip();
  Tooltip(const Tooltip&) = delete;
  Tooltip& operator=(const Tooltip&) = delete;

  // Opens above `iconRect` (screen coordinates), centred on it and
  // clamped into its monitor. An empty tip hides the window.
  void Show(const std::wstring& text, const RECT& iconRect);
  void Hide();
  [[nodiscard]] HWND hwnd() const noexcept { return hwnd_; }

 private:
  Tooltip() = default;

  static bool EnsureClass();
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam,
                                  LPARAM lparam);
  // Selects the status font, wraps the text to the measured width and
  // returns the box size in pixels.
  bool Measure(const std::wstring& text, int dpi, int maxWidth, int maxHeight,
               int* width, int* height);
  void Paint(HDC dc);
  [[nodiscard]] bool UsesDarkTheme() const;

  HWND owner_ = nullptr;
  HWND hwnd_ = nullptr;
  ThemeProbe dark_;
  bool visible_ = false;
  int dpi_ = 96;
  RECT anchor_ = {};
  HFONT font_ = nullptr;
  std::wstring text_;
  std::wstring wrapped_;
};

}  // namespace traymirror
