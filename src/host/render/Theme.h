// Fallback WinUI colors and scalable spacing. Runtime shell bounds, system
// font metrics, high-contrast colors and DWM material take precedence.
#pragma once

#include <windows.h>

#include <d2d1.h>

namespace traymirror {

struct ThemeTokens {
  // SolidBackgroundFillColorBase: #202020 dark, #F3F3F3 light.
  COLORREF stripBackground = RGB(0x20, 0x20, 0x20);
  // SolidBackgroundFillColorQuarternary: #2C2C2C dark, #FFFFFF light.
  // WinUI paints a tooltip with acrylic; this is the opaque surface the
  // acrylic resolves to when transparency is off.
  COLORREF surface = RGB(0x2C, 0x2C, 0x2C);
  // TextFillColorPrimary: #FFFFFF dark, #E4000000 light.
  D2D1_COLOR_F text = D2D1::ColorF(0xFFFFFF);
  // Measured Win11 tray tooltip: a solid surface, not a system backdrop.
  // #292D32 with #F7F7F7 text (dark), #F9F9F9 with #1A1A1A (light).
  COLORREF tooltipSurface = RGB(0x29, 0x2D, 0x32);
  D2D1_COLOR_F tooltipText = D2D1::ColorF(0xF7F7F7);
  // SubtleFillColorSecondary: #0FFFFFFF dark, #09000000 light.
  D2D1_COLOR_F hover = D2D1::ColorF(0xFFFFFF, 0x0F / 255.0F);

  [[nodiscard]] static ThemeTokens For(bool dark);
  [[nodiscard]] static ThemeTokens Current(bool dark);
};

// Windows mode, not the independent app-mode personalization setting.
bool SystemUsesDarkTheme();
bool SystemUsesHighContrast();
// DWM owns popup corners and shadow. The measured tooltip is opaque, so
// no system backdrop is requested; it paints its own solid surface.
void ApplyPopupAppearance(HWND hwnd, bool dark, bool compact = false);

namespace metrics {

// Fallback control geometry in DIPs; popup corners are owned by DWM.
constexpr int kControlCornerRadius = 4;

// The measured tooltip padding, ink to border: 10 left, 11 right, 11
// top, 9 bottom. These constants are border to the GDI text cell, whose
// trailing bearing is about 1 px and whose ink starts about 3 px below
// the top; the totals (20 x 16) reproduce the measured 212 x 31 box at
// 96 DPI with the status font's 15 px cell.
constexpr int kTooltipPadLeft = 10;
constexpr int kTooltipPadTop = 8;
constexpr int kTooltipPadRight = 10;
constexpr int kTooltipPadBottom = 8;

// Distance from a flyout to the edge it opens against.
constexpr int kFlyoutGap = 8;
// Measured gap from the tooltip's bottom to the taskbar top.
constexpr int kTooltipGap = 9;

// The notification area is not part of the design system: the tray
// fallback button is 32 x 32 around a 16 px glyph, with 8 DIP padding.
// Runtime primary-tray bounds override this fallback.
// Windows highlights the whole button, not the glyph.
constexpr int kIconPad = 8;

}  // namespace metrics

// DIPs to pixels at `dpi`: the single scaling rule the overlay uses.
int Px(int dip, int dpi);

}  // namespace traymirror
