// The Win11-style tooltip (spec 6.4, report 09): our own layered window
// above the hovered icon, with the measured padding, radius and colours.
#include "render/Tooltip.h"

#include "win/Monitors.h"

#include <windows.h>

#include <gtest/gtest.h>

namespace {

using traymirror::Tooltip;
using traymirror::TooltipPalette;

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

// The palette is the theme-dependent half of the appearance; the numbers
// are the measured ones from docs/visual-spec.md.
TEST(Tooltip, FollowsTheTheme) {
  const TooltipPalette dark = TooltipPalette::For(true);
  const TooltipPalette light = TooltipPalette::For(false);

  EXPECT_EQ(dark.background, RGB(41, 45, 50));   // measured #292D32
  EXPECT_EQ(dark.text, RGB(247, 247, 247));      // measured #F7F7F7
  EXPECT_EQ(light.background, RGB(249, 249, 249));
  EXPECT_EQ(light.text, RGB(26, 26, 26));
  EXPECT_NE(dark.background, light.background);
}
