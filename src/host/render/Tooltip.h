// Tooltip for one overlay strip (spec 6.4, report 09): our own layered
// window with the measured Win11 geometry — rounded corners, 10 px
// horizontal padding, a 9 px gap above the icon, theme colours. Drawn
// with Direct2D into a DIB and handed to the window as a premultiplied
// layer, so the corners are anti-aliased against whatever is behind it.
#pragma once

#include <windows.h>

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <functional>
#include <memory>
#include <string>

namespace traymirror {

// The theme half of the appearance, in the units the measurement used.
// Values are the cells recorded in docs/visual-spec.md.
struct TooltipPalette {
  COLORREF background = 0;
  COLORREF text = 0;
  [[nodiscard]] static TooltipPalette For(bool dark);
};

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
  bool InitFactories();
  bool Measure(const std::wstring& text, int dpi, int* width, int* height);
  // Draws the box and the text and hands the pixels to the layered
  // window at `destination` (screen coordinates).
  bool Paint(const std::wstring& text, int width, int height, int dpi,
             POINT destination);
  [[nodiscard]] bool UsesDarkTheme() const;

  HWND owner_ = nullptr;
  HWND hwnd_ = nullptr;
  ThemeProbe dark_;
  bool visible_ = false;
  Microsoft::WRL::ComPtr<ID2D1Factory> d2d_;
  Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> format_;
  Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> target_;
};

}  // namespace traymirror
