// The overflow flyout (spec 6.4, plan rulings R1/R7): the chevron at the
// strip's right end opens a grid of the icons the strip does not show;
// a click on a cell is replayed to the owner, Esc and an outside click
// close the window, and closing destroys it rather than hiding it.
#include "render/Flyout.h"
#include "render/CellLayout.h"
#include "render/Overlay.h"

#include "dispatch/Dispatch.h"
#include "win/Monitors.h"

#include <windows.h>

#include <commctrl.h>

#include <gtest/gtest.h>

#include <iterator>
#include <vector>

namespace {

using traymirror::ComputeOverlayStripRect;
using traymirror::DispatchCall;
using traymirror::DispatchTarget;
using traymirror::Flyout;
using traymirror::GridLayout;
using traymirror::IconPxForDpi;
using traymirror::IconView;
using traymirror::LayoutGrid;
using traymirror::LayoutStrip;
using traymirror::MonitorTaskbar;
using traymirror::Overlay;
using traymirror::StripLayout;

constexpr UINT kClickCallback = WM_APP + 210;
constexpr wchar_t kFlyoutClass[] = L"TrayMirrorFlyout";

MonitorTaskbar SyntheticTaskbar() {
  const POINT origin = {0, 0};
  const HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO mi = {};
  mi.cbSize = sizeof(mi);
  EXPECT_NE(GetMonitorInfoW(monitor, &mi), FALSE);
  MonitorTaskbar mt;
  mt.monitor = monitor;
  mt.monitorRect = mi.rcMonitor;
  mt.taskbarRect = {mi.rcMonitor.left, mi.rcWork.bottom, mi.rcMonitor.right,
                    mi.rcMonitor.bottom};
  mt.clockRect = {mt.taskbarRect.right - 73, mt.taskbarRect.top,
                  mt.taskbarRect.right, mt.taskbarRect.bottom};
  mt.hasTaskbar = true;
  return mt;
}

DispatchTarget MakeTarget(HWND owner) {
  return DispatchTarget{.owner = owner,
                        .callbackMessage = kClickCallback,
                        .uid = 9,
                        .version = 4,
                        .ownerPid = 4242};
}

IconView FirstIcon(DispatchTarget target, const wchar_t* tip = L"hidden") {
  return IconView{.icon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512)),
                  .serial = 1,
                  .tip = tip,
                  .target = target};
}

struct WindowSearch {
  const wchar_t* name = nullptr;
  HWND found = nullptr;
};

BOOL CALLBACK FindWindowProc(HWND hwnd, LPARAM data) {
  auto* search = reinterpret_cast<WindowSearch*>(data);
  wchar_t cls[64] = {};
  if (GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls))) != 0 &&
      wcscmp(cls, search->name) == 0) {
    search->found = hwnd;
    return FALSE;
  }
  return TRUE;
}

HWND FirstWindowOfClass(const wchar_t* name) {
  WindowSearch search;
  search.name = name;
  EnumWindows(FindWindowProc, reinterpret_cast<LPARAM>(&search));
  return search.found;
}

}  // namespace

// The chevron exists only when something is hidden under it, and it is
// the strip's own cell: a click opens the flyout above the strip.
TEST(FlyoutIntegration, ChevronClickOpensFlyoutAboveTheStrip) {
  const MonitorTaskbar mt = SyntheticTaskbar();
  const RECT strip = ComputeOverlayStripRect(mt, 400);
  auto overlay = Overlay::Create(mt.monitor, strip, {},
                                 [](const DispatchCall&) {});
  ASSERT_NE(overlay, nullptr);
  overlay->SetIcons({FirstIcon(MakeTarget(reinterpret_cast<HWND>(0x1)))},
                    {FirstIcon(MakeTarget(reinterpret_cast<HWND>(0x2)))});
  overlay->Render();

  UINT dpi = GetDpiForWindow(overlay->hwnd());
  if (dpi == 0) {
    dpi = 96;
  }
  const int iconPx = IconPxForDpi(dpi);
  const int gap = traymirror::IconGapPxForDpi(dpi);
  const StripLayout layout =
      LayoutStrip(strip.right - strip.left, strip.bottom - strip.top, iconPx,
                  gap, 1, true);
  ASSERT_TRUE(layout.hasChevron);
  const POINT pt = {(layout.chevron.left + layout.chevron.right) / 2,
                    (layout.chevron.top + layout.chevron.bottom) / 2};
  SendMessageW(overlay->hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(pt.x, pt.y));

  const HWND flyout = FirstWindowOfClass(kFlyoutClass);
  ASSERT_NE(flyout, nullptr);
  RECT rect = {};
  ASSERT_NE(GetWindowRect(flyout, &rect), FALSE);
  EXPECT_LE(rect.bottom, strip.top);  // above the strip
  EXPECT_LE(rect.right, strip.right);
  EXPECT_NE(IsWindowVisible(flyout), FALSE);
  SendMessageW(flyout, WM_KEYDOWN, VK_ESCAPE, 0);
}

// Nothing hidden: no chevron, and a click where it would have been opens
// nothing.
TEST(FlyoutIntegration, NoChevronAndNoFlyoutWithoutHiddenIcons) {
  const MonitorTaskbar mt = SyntheticTaskbar();
  const RECT strip = ComputeOverlayStripRect(mt, 400);
  auto overlay = Overlay::Create(mt.monitor, strip, {},
                                 [](const DispatchCall&) {});
  ASSERT_NE(overlay, nullptr);
  overlay->SetIcons({FirstIcon(MakeTarget(reinterpret_cast<HWND>(0x1)))}, {});
  overlay->Render();

  UINT dpi = GetDpiForWindow(overlay->hwnd());
  if (dpi == 0) {
    dpi = 96;
  }
  const int iconPx = IconPxForDpi(dpi);
  const int x = strip.right - strip.left - iconPx;
  const int y = (strip.bottom - strip.top) / 2;
  SendMessageW(overlay->hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(x, y));

  EXPECT_EQ(FirstWindowOfClass(kFlyoutClass), nullptr);
}

// A click on a flyout cell is the same gesture a strip icon would get.
TEST(FlyoutIntegration, FlyoutCellClickDispatches) {
  const MonitorTaskbar mt = SyntheticTaskbar();
  const RECT strip = ComputeOverlayStripRect(mt, 400);
  std::vector<DispatchCall> calls;
  bool closed = false;
  auto flyout = Flyout::Create(
      mt.monitor, strip,
      {FirstIcon(MakeTarget(reinterpret_cast<HWND>(0xA1)), L"one"),
       FirstIcon(MakeTarget(reinterpret_cast<HWND>(0xB2)), L"two")},
      [&](const DispatchCall& call) { calls.push_back(call); },
      [&]() { closed = true; });
  ASSERT_NE(flyout, nullptr);
  const HWND hwnd = flyout->hwnd();

  const GridLayout grid = LayoutGrid(IconPxForDpi(96), 2);
  ASSERT_EQ(grid.cells.size(), 2U);
  const RECT& cell = grid.cells[1];  // the second icon
  const POINT pt = {(cell.left + cell.right) / 2, (cell.top + cell.bottom) / 2};
  SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(pt.x, pt.y));
  SendMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(pt.x, pt.y));

  EXPECT_TRUE(closed);
  EXPECT_EQ(IsWindow(hwnd), FALSE);  // destroyed, not hidden (R7)
  ASSERT_EQ(calls.size(), 2U);
  EXPECT_EQ(calls[0].event, traymirror::TrayEvent::kLeftDown);
  EXPECT_EQ(calls[1].event, traymirror::TrayEvent::kLeftUp);
  EXPECT_EQ(calls[0].target.owner, reinterpret_cast<HWND>(0xB2));
}

TEST(FlyoutIntegration, EscapeClosesAndDestroysTheFlyout) {
  const MonitorTaskbar mt = SyntheticTaskbar();
  const RECT strip = ComputeOverlayStripRect(mt, 400);
  bool closed = false;
  auto flyout = Flyout::Create(mt.monitor, strip,
                               {FirstIcon(MakeTarget(reinterpret_cast<HWND>(0xA1)))},
                               [](const DispatchCall&) {},
                               [&]() { closed = true; });
  ASSERT_NE(flyout, nullptr);
  const HWND hwnd = flyout->hwnd();

  SendMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, 0);

  EXPECT_TRUE(closed);
  EXPECT_EQ(IsWindow(hwnd), FALSE);
  EXPECT_EQ(FirstWindowOfClass(kFlyoutClass), nullptr);
}

// An outside click deactivates the flyout; that has to close it too.
TEST(FlyoutIntegration, OutsideClickDeactivationClosesTheFlyout) {
  const MonitorTaskbar mt = SyntheticTaskbar();
  const RECT strip = ComputeOverlayStripRect(mt, 400);
  bool closed = false;
  auto flyout = Flyout::Create(mt.monitor, strip,
                               {FirstIcon(MakeTarget(reinterpret_cast<HWND>(0xA1)))},
                               [](const DispatchCall&) {},
                               [&]() { closed = true; });
  ASSERT_NE(flyout, nullptr);
  const HWND hwnd = flyout->hwnd();

  SendMessageW(hwnd, WM_ACTIVATE, MAKEWPARAM(WA_ACTIVE, 0), 0);
  EXPECT_FALSE(closed);
  SendMessageW(hwnd, WM_ACTIVATE, MAKEWPARAM(WA_INACTIVE, 0), 0);

  EXPECT_TRUE(closed);
  EXPECT_EQ(IsWindow(hwnd), FALSE);
}
