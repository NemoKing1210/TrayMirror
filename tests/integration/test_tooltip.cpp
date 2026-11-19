// The Win11-style tooltip (spec 6.4, report 09): our own layered window
// above the hovered icon, with the measured padding, radius and colours.
#include "render/Tooltip.h"
#include "render/Theme.h"

#include "win/Monitors.h"

#include <windows.h>
#include <dwmapi.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

namespace {

using traymirror::Tooltip;

// The exact tip captured in spike 09; the measured box for it is 212 x 31
// at 96 DPI (docs/visual-spec.md, tooltip section).
constexpr wchar_t kMeasuredTip[] = L"EarTrumpet: 25% - Наушники (USB)";

// Paints the window's client area into an off-screen BGRA DIB through the
// same Paint() path WM_PAINT uses, so pixels can be asserted without a
// screen capture.
std::vector<DWORD> CaptureClient(HWND hwnd, int width, int height) {
  BITMAPINFO info = {};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;  // top-down
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  const HDC dc = CreateCompatibleDC(nullptr);
  const HBITMAP dib =
      CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (dc == nullptr || dib == nullptr) {
    if (dib != nullptr) {
      DeleteObject(dib);
    }
    if (dc != nullptr) {
      DeleteDC(dc);
    }
    return {};
  }
  const HGDIOBJ previous = SelectObject(dc, dib);
  SendMessageW(hwnd, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
  SelectObject(dc, previous);
  const auto* pixels = static_cast<const DWORD*>(bits);
  std::vector<DWORD> out(pixels, pixels + (width * height));
  DeleteObject(dib);
  DeleteDC(dc);
  return out;
}

int Channel(DWORD pixel, int shift) {
  return static_cast<int>((pixel >> shift) & 0xFFU);
}

HWND CreateOwner() {
  constexpr wchar_t kClass[] = L"TrayMirrorTooltipTestOwner";
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = kClass;
  RegisterClassExW(&wc);
  return CreateWindowExW(0, kClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                         wc.hInstance, nullptr);
}

// The strip sits over the taskbar; the tooltip opens above it.
RECT IconRectOnTaskbar() {
  const POINT origin = {0, 0};
  const HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO mi = {};
  mi.cbSize = sizeof(mi);
  EXPECT_NE(GetMonitorInfoW(monitor, &mi), FALSE);
  const int size = traymirror::IconPxForDpi(96);
  const int x = mi.rcMonitor.right - 300;
  const int top = mi.rcWork.bottom + ((mi.rcMonitor.bottom - mi.rcWork.bottom -
                                       size) / 2);
  return RECT{.left = x, .top = top, .right = x + size, .bottom = top + size};
}

}  // namespace

TEST(Tooltip, SitsAboveTheIconAndInsideTheMonitor) {
  const HWND owner = CreateOwner();
  ASSERT_NE(owner, nullptr);
  auto tip = Tooltip::Create(owner, []() { return true; });
  ASSERT_NE(tip, nullptr);
  const RECT icon = IconRectOnTaskbar();

  tip->Show(L"EarTrumpet: 25%", icon);

  RECT rect = {};
  ASSERT_NE(GetWindowRect(tip->hwnd(), &rect), FALSE);
  EXPECT_NE(IsWindowVisible(tip->hwnd()), FALSE);
  EXPECT_LE(rect.bottom, icon.top);  // above the icon, never over it

  const POINT origin = {0, 0};
  MONITORINFO mi = {};
  mi.cbSize = sizeof(mi);
  ASSERT_NE(GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY),
                            &mi),
            FALSE);
  EXPECT_GE(rect.left, mi.rcMonitor.left);
  EXPECT_LE(rect.right, mi.rcMonitor.right);
  EXPECT_GE(rect.top, mi.rcMonitor.top);

  // Centred on the icon while there is room (measured: centre 2359.5 for
  // an icon at 2360).
  EXPECT_NEAR((rect.left + rect.right) / 2, (icon.left + icon.right) / 2, 1);
  EXPECT_GT(rect.right - rect.left, rect.bottom - rect.top);

  DestroyWindow(owner);
}

// A longer tip needs a wider window; the text is not clipped.
TEST(Tooltip, TooltipKeepsTheTextOfTheHoveredIcon) {
  const HWND owner = CreateOwner();
  ASSERT_NE(owner, nullptr);
  auto tip = Tooltip::Create(owner, []() { return true; });
  ASSERT_NE(tip, nullptr);
  const RECT icon = IconRectOnTaskbar();

  tip->Show(L"one", icon);
  RECT narrow = {};
  ASSERT_NE(GetWindowRect(tip->hwnd(), &narrow), FALSE);

  tip->Show(L"a much longer tooltip text than the first one", icon);
  RECT wide = {};
  ASSERT_NE(GetWindowRect(tip->hwnd(), &wide), FALSE);

  EXPECT_GT(wide.right - wide.left, narrow.right - narrow.left);
  EXPECT_GT(wide.right - wide.left, 100);  // it fits the whole string

  DestroyWindow(owner);
}

TEST(Tooltip, EmptyTipShowsNothing) {
  const HWND owner = CreateOwner();
  ASSERT_NE(owner, nullptr);
  auto tip = Tooltip::Create(owner, []() { return true; });
  ASSERT_NE(tip, nullptr);
  const RECT icon = IconRectOnTaskbar();

  tip->Show(L"something", icon);
  ASSERT_NE(IsWindowVisible(tip->hwnd()), FALSE);
  tip->Show(L"", icon);

  EXPECT_EQ(IsWindowVisible(tip->hwnd()), FALSE);
  DestroyWindow(owner);
}

// The measured profile: 212 x 31 px at 96 DPI for the captured tip,
// 21 px horizontal and 15 px vertical of non-ink space (spec 6.4).
TEST(Tooltip, SampleTipMatchesTheMeasuredBox) {
  const HWND owner = CreateOwner();
  ASSERT_NE(owner, nullptr);
  auto tip = Tooltip::Create(owner, []() { return true; });
  ASSERT_NE(tip, nullptr);
  const int dpi = static_cast<int>(GetDpiForWindow(owner));
  ASSERT_GE(dpi, 96);

  tip->Show(kMeasuredTip, IconRectOnTaskbar());

  RECT rect = {};
  ASSERT_NE(GetWindowRect(tip->hwnd(), &rect), FALSE);
  EXPECT_NEAR(rect.right - rect.left, MulDiv(212, dpi, 96), 2);
  EXPECT_NEAR(rect.bottom - rect.top, MulDiv(31, dpi, 96), 2);
  tip.reset();
  DestroyWindow(owner);
}

// The tooltip is a solid measured surface, not the non-transmitting DWM
// backdrop the old implementation asked for and then cleared transparent.
TEST(Tooltip, PaintsTheMeasuredSurface) {
  if (traymirror::SystemUsesHighContrast()) {
    GTEST_SKIP() << "High contrast replaces the measured tokens";
  }
  const HWND owner = CreateOwner();
  ASSERT_NE(owner, nullptr);
  const RECT icon = IconRectOnTaskbar();
  for (const bool dark : {true, false}) {
    auto tip = Tooltip::Create(owner, [dark]() { return dark; });
    ASSERT_NE(tip, nullptr);
    tip->Show(kMeasuredTip, icon);
    RECT rect = {};
    ASSERT_NE(GetWindowRect(tip->hwnd(), &rect), FALSE);
    const std::vector<DWORD> pixels =
        CaptureClient(tip->hwnd(), rect.right - rect.left, rect.bottom - rect.top);
    ASSERT_EQ(pixels.size(),
              static_cast<size_t>((rect.right - rect.left) *
                                  (rect.bottom - rect.top)));
    const DWORD corner = pixels[0];
    const int red = dark ? 0x29 : 0xF9;
    const int green = dark ? 0x2D : 0xF9;
    const int blue = dark ? 0x32 : 0xF9;
    EXPECT_NEAR(Channel(corner, 16), red, 2) << "dark=" << dark;
    EXPECT_NEAR(Channel(corner, 8), green, 2) << "dark=" << dark;
    EXPECT_NEAR(Channel(corner, 0), blue, 2) << "dark=" << dark;
  }
  DestroyWindow(owner);
}

// The measured text colours, read off the brightest ink pixel.
TEST(Tooltip, PaintsTheMeasuredTextColour) {
  if (traymirror::SystemUsesHighContrast()) {
    GTEST_SKIP() << "High contrast replaces the measured tokens";
  }
  const HWND owner = CreateOwner();
  ASSERT_NE(owner, nullptr);
  const RECT icon = IconRectOnTaskbar();
  for (const bool dark : {true, false}) {
    auto tip = Tooltip::Create(owner, [dark]() { return dark; });
    ASSERT_NE(tip, nullptr);
    tip->Show(kMeasuredTip, icon);
    RECT rect = {};
    ASSERT_NE(GetWindowRect(tip->hwnd(), &rect), FALSE);
    const std::vector<DWORD> pixels =
        CaptureClient(tip->hwnd(), rect.right - rect.left, rect.bottom - rect.top);
    ASSERT_FALSE(pixels.empty());
    DWORD brightest = 0;
    int best = -1;
    for (const DWORD pixel : pixels) {
      const int value = Channel(pixel, 16) + Channel(pixel, 8) + Channel(pixel, 0);
      if (value > best) {
        best = value;
        brightest = pixel;
      }
    }
    // Dark theme text is #F7F7F7; light theme text is #1A1A1A, so the
    // brightest pixel is the surface for light and the ink for dark.
    const int ink = dark ? 0xF7 : 0x1A;
    if (dark) {
      EXPECT_NEAR(Channel(brightest, 16), ink, 3) << "dark=" << dark;
      EXPECT_NEAR(Channel(brightest, 8), ink, 3) << "dark=" << dark;
      EXPECT_NEAR(Channel(brightest, 0), ink, 3) << "dark=" << dark;
    } else {
      DWORD darkest = 0xFFFFFF;
      int lowest = 256 * 3;
      for (const DWORD pixel : pixels) {
        const int value =
            Channel(pixel, 16) + Channel(pixel, 8) + Channel(pixel, 0);
        if (value < lowest) {
          lowest = value;
          darkest = pixel;
        }
      }
      EXPECT_NEAR(Channel(darkest, 16), ink, 3) << "dark=" << dark;
      EXPECT_NEAR(Channel(darkest, 8), ink, 3) << "dark=" << dark;
      EXPECT_NEAR(Channel(darkest, 0), ink, 3) << "dark=" << dark;
    }
  }
  DestroyWindow(owner);
}

TEST(Tooltip, LongTextStaysInsideItsMonitor) {
  const HWND owner = CreateOwner();
  ASSERT_NE(owner, nullptr);
  auto tip = Tooltip::Create(owner);
  ASSERT_NE(tip, nullptr);
  const RECT icon = IconRectOnTaskbar();
  tip->Show(std::wstring(600, L'W'), icon);
  RECT actual = {};
  ASSERT_NE(GetWindowRect(tip->hwnd(), &actual), FALSE);
  MONITORINFO mi = {};
  mi.cbSize = sizeof(mi);
  RECT probe = icon;
  ASSERT_NE(GetMonitorInfoW(MonitorFromRect(&probe, MONITOR_DEFAULTTONEAREST), &mi), FALSE);
  EXPECT_GE(actual.left, mi.rcMonitor.left);
  EXPECT_LE(actual.right, mi.rcMonitor.right);
  EXPECT_GE(actual.top, mi.rcMonitor.top);
  EXPECT_LE(actual.bottom, mi.rcMonitor.bottom);
  tip.reset();
  DestroyWindow(owner);
}

TEST(Tooltip, HideRemovesTheWindowFromTheScreen) {
  const HWND owner = CreateOwner();
  ASSERT_NE(owner, nullptr);
  auto tip = Tooltip::Create(owner, []() { return true; });
  ASSERT_NE(tip, nullptr);

  tip->Show(L"hovered", IconRectOnTaskbar());
  ASSERT_NE(IsWindowVisible(tip->hwnd()), FALSE);
  tip->Hide();

  EXPECT_EQ(IsWindowVisible(tip->hwnd()), FALSE);
  DestroyWindow(owner);
}

// The renderer must apply the selected mode to the actual DWM window,
// rather than merely return a matching set of hardcoded colors.
TEST(Tooltip, ThemeProbeUpdatesTheNativeWindow) {
  std::unique_ptr<HWND__, decltype(&DestroyWindow)> owner(CreateOwner(), DestroyWindow);
  ASSERT_NE(owner, nullptr);
  for (const bool dark : {true, false}) {
    auto tip = Tooltip::Create(owner.get(), [dark]() { return dark; });
    ASSERT_NE(tip, nullptr);
    tip->Show(L"native mode", IconRectOnTaskbar());
    BOOL actual = FALSE;
    const HRESULT result = DwmGetWindowAttribute(tip->hwnd(), DWMWA_USE_IMMERSIVE_DARK_MODE,
                                                  &actual, sizeof(actual));
    if (FAILED(result)) {
      GTEST_SKIP() << "The compositor does not expose immersive mode";
    }
    EXPECT_EQ(actual != FALSE, dark);
    DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_DEFAULT;
    ASSERT_EQ(DwmGetWindowAttribute(tip->hwnd(), DWMWA_WINDOW_CORNER_PREFERENCE,
                                     &corners, sizeof(corners)), S_OK);
    EXPECT_EQ(corners, DWMWCP_ROUNDSMALL);
  }
}

TEST(Tooltip, TooltipDoesNotInterceptPointerInput) {
  std::unique_ptr<HWND__, decltype(&DestroyWindow)> owner(CreateOwner(), DestroyWindow);
  auto tip = Tooltip::Create(owner.get());
  ASSERT_NE(tip, nullptr);
  tip->Show(L"non-interactive", IconRectOnTaskbar());
  RECT rect = {};
  ASSERT_NE(GetWindowRect(tip->hwnd(), &rect), FALSE);
  EXPECT_EQ(SendMessageW(tip->hwnd(), WM_NCHITTEST, 0,
                         MAKELPARAM(rect.left + 4, rect.top + 4)), HTTRANSPARENT);
}
