#include "render/Theme.h"

#include <dwmapi.h>

namespace traymirror {

bool SystemUsesHighContrast() {
  HIGHCONTRASTW contrast = {};
  contrast.cbSize = sizeof(contrast);
  return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast),
                               &contrast, 0) != FALSE &&
         (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

bool SystemUsesDarkTheme() {
  DWORD light = 1;
  DWORD size = sizeof(light);
  if (RegGetValueW(HKEY_CURRENT_USER,
                   L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                   L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr,
                   &light, &size) == ERROR_SUCCESS) {
    return light == 0;
  }
  const COLORREF color = GetSysColor(COLOR_WINDOW);
  return GetRValue(color) + GetGValue(color) + GetBValue(color) < 384;
}

void ApplyPopupAppearance(HWND hwnd, bool dark, bool compact) {
  const BOOL immersive = dark ? TRUE : FALSE;
  DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE,
                         &immersive, sizeof(immersive));
  const DWM_WINDOW_CORNER_PREFERENCE corner =
      compact ? DWMWCP_ROUNDSMALL : DWMWCP_ROUND;
  DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner,
                         sizeof(corner));
  // The measured tooltip transmits nothing: a DWM backdrop replaces the
  // whole client with a wallpaper-derived fill. It paints its own opaque
  // surface, so requesting a backdrop would only override the pixels.
}

ThemeTokens ThemeTokens::For(bool dark) {
  ThemeTokens t;
  if (dark) {
    return t;  // the defaults above are the dark theme
  }
  t.stripBackground = RGB(0xF3, 0xF3, 0xF3);  // SolidBackgroundFillColorBase
  t.surface = RGB(0xFF, 0xFF, 0xFF);          // ...Quarternary
  t.tooltipSurface = RGB(0xF9, 0xF9, 0xF9);
  t.tooltipText = D2D1::ColorF(0x1A1A1A);
  t.text = D2D1::ColorF(0x000000, 0xE4 / 255.0F);          // TextFillColorPrimary
  t.hover = D2D1::ColorF(0x000000, 0x09 / 255.0F);         // SubtleFillColorSecondary
  return t;
}

ThemeTokens ThemeTokens::Current(bool dark) {
  ThemeTokens tokens = For(dark);
  if (SystemUsesHighContrast()) {
    const auto color = [](int index) {
      const COLORREF value = GetSysColor(index);
      return D2D1::ColorF(GetRValue(value) / 255.0F,
                         GetGValue(value) / 255.0F,
                         GetBValue(value) / 255.0F);
    };
    tokens.surface = GetSysColor(COLOR_WINDOW);
    tokens.stripBackground = GetSysColor(COLOR_BTNFACE);
    tokens.text = color(COLOR_WINDOWTEXT);
    tokens.tooltipSurface = GetSysColor(COLOR_WINDOW);
    tokens.tooltipText = color(COLOR_WINDOWTEXT);
    tokens.hover = color(COLOR_HIGHLIGHT);
  }
  return tokens;
}

int Px(int dip, int dpi) { return MulDiv(dip, dpi, 96); }

}  // namespace traymirror
