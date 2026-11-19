// The overflow flyout (spec 6.4, plan rulings R1/R7): the chevron at the
// strip's right end opens a grid of the icons the strip does not show;
// a click on a cell is replayed to the owner, Esc and an outside click
// close the window, and closing destroys it rather than hiding it.
#include "render/Flyout.h"
#include "render/CellLayout.h"
#include "render/NativePopupSurface.h"
#include "render/Overlay.h"
#include "render/PopupTransition.h"
#include "render/Theme.h"

#include "dispatch/Dispatch.h"
#include "win/Monitors.h"

#include <windows.h>

#include <commctrl.h>

#include <gtest/gtest.h>

#include <iterator>
#include <string>
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
using traymirror::PopupTransition;
using traymirror::StripLayout;
using traymirror::TrayEvent;

constexpr UINT kClickCallback = WM_APP + 210;
constexpr wchar_t kFlyoutClass[] = L"TrayMirrorFlyout";
constexpr wchar_t kTooltipClass[] = L"TrayMirrorTooltip";

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
  mt.hasTaskListBounds = true;
  mt.taskListRight = mi.rcMonitor.left;
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
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != GetCurrentProcessId()) {
    return TRUE;
  }
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

// The visible tooltip window, or null when none is shown.
HWND VisibleTooltip() {
  const HWND tip = FirstWindowOfClass(kTooltipClass);
  return tip != nullptr && IsWindowVisible(tip) != FALSE ? tip : nullptr;
}

std::wstring TooltipText(HWND tip) {
  wchar_t text[512] = {};
  GetWindowTextW(tip, text, static_cast<int>(std::size(text)));
  return text;
}

// Both the class cursor and an explicit client hit must be the arrow.
void ExpectArrowCursor(HWND hwnd) {
  const HCURSOR arrow = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  EXPECT_EQ(GetClassLongPtrW(hwnd, GCLP_HCURSOR),
            reinterpret_cast<LONG_PTR>(arrow));
  SetCursor(nullptr);
  const LRESULT handled = SendMessageW(
      hwnd, WM_SETCURSOR, reinterpret_cast<WPARAM>(hwnd),
      MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
  EXPECT_EQ(handled, TRUE);
  EXPECT_EQ(GetCursor(), arrow);
}

// The constant alpha of the layered flyout window; -1 when the window is
// not layered, 255 when no per-window alpha was ever set.
int LayeredAlpha(HWND hwnd) {
  BYTE alpha = 0;
  COLORREF color = 0;
  DWORD flags = 0;
  if (GetLayeredWindowAttributes(hwnd, &color, &alpha, &flags) == FALSE) {
    return -1;
  }
  return flags & LWA_ALPHA ? alpha : 255;
}

// Drives the animation timer deterministically: tests do not pump the
// message loop, so a posted WM_TIMER would never be dispatched. The fade
// completes in 167 ms; 40 x 18 ms is well beyond it and stops as soon as
// the window is destroyed.
void FinishTransition(HWND hwnd) {
  for (int i = 0; i < 40 && IsWindow(hwnd) != FALSE; ++i) {
    Sleep(static_cast<DWORD>(traymirror::PopupTransition::kIntervalMs + 2));
    SendMessageW(hwnd, WM_TIMER, 0, 0);
  }
}

bool AnimationsEnabled() { return traymirror::SystemAnimationsEnabled(); }

POINT ChevronPoint(const RECT& strip, int dpi) {
  const int iconPx = IconPxForDpi(static_cast<UINT>(dpi));
  const int gap = MulDiv(8, dpi, 96);
  const StripLayout layout =
      LayoutStrip(strip.right - strip.left, strip.bottom - strip.top, iconPx,
                  gap, 1, true);
  return POINT{.x = (layout.chevron.left + layout.chevron.right) / 2,
               .y = (layout.chevron.top + layout.chevron.bottom) / 2};
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
  const int gap = MulDiv(8, static_cast<int>(dpi), 96);
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
  FinishTransition(flyout);
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

TEST(FlyoutIntegration, ClippedPromotedIconsRemainReachableThroughTheChevron) {
  const MonitorTaskbar mt = SyntheticTaskbar();
  const RECT strip{100, 100, 164, 148};
  auto overlay = Overlay::Create(mt.monitor, strip, {}, [](const DispatchCall&) {});
  ASSERT_NE(overlay, nullptr);
  overlay->SetIcons({FirstIcon(MakeTarget(reinterpret_cast<HWND>(0x1))),
                     FirstIcon(MakeTarget(reinterpret_cast<HWND>(0x2))),
                     FirstIcon(MakeTarget(reinterpret_cast<HWND>(0x3)))});
  overlay->Render();
  // The native chevron is the leftmost button.
  SendMessageW(overlay->hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(16, 24));
  const HWND flyout = FirstWindowOfClass(kFlyoutClass);
  ASSERT_NE(flyout, nullptr);
  SendMessageW(flyout, WM_KEYDOWN, VK_ESCAPE, 0);
  FinishTransition(flyout);
}

TEST(FlyoutIntegration, FullButtonPaddingDispatchesToTheIconOwner) {
  const MonitorTaskbar mt = SyntheticTaskbar();
  std::vector<DispatchCall> calls;
  auto flyout = Flyout::Create(mt.monitor, ComputeOverlayStripRect(mt, 200),
      {FirstIcon(MakeTarget(reinterpret_cast<HWND>(0xA1)))},
      [&](const DispatchCall& call) { calls.push_back(call); }, []() {});
  ASSERT_NE(flyout, nullptr);
  SendMessageW(flyout->hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(30, 24));
  ASSERT_EQ(calls.size(), 1U);
  EXPECT_EQ(calls[0].target.owner, reinterpret_cast<HWND>(0xA1));
}

TEST(FlyoutIntegration, AnimatedHiddenIconsDoNotCloseThePanel) {
  const MonitorTaskbar mt = SyntheticTaskbar();
  auto overlay = Overlay::Create(mt.monitor, ComputeOverlayStripRect(mt, 200));
  ASSERT_NE(overlay, nullptr);
  IconView hidden = FirstIcon(MakeTarget(reinterpret_cast<HWND>(0xA1)));
  overlay->SetIcons({}, {hidden});
  overlay->Render();
  SendMessageW(overlay->hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(16, 24));
  const HWND flyout = FirstWindowOfClass(kFlyoutClass);
  ASSERT_NE(flyout, nullptr);
  ++hidden.serial;
  hidden.tip = L"updated";
  overlay->SetIcons({}, {hidden});
  overlay->Render();
  EXPECT_NE(IsWindow(flyout), FALSE);
  EXPECT_NE(IsWindowVisible(overlay->hwnd()), FALSE);
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
  FinishTransition(hwnd);

  EXPECT_TRUE(closed);
  EXPECT_EQ(IsWindow(hwnd), FALSE);  // destroyed, not hidden (R7)
  ASSERT_EQ(calls.size(), 2U);
  EXPECT_EQ(calls[0].event, traymirror::TrayEvent::kLeftDown);
  EXPECT_EQ(calls[1].event, traymirror::TrayEvent::kLeftUp);
  EXPECT_EQ(calls[0].target.owner, reinterpret_cast<HWND>(0xB2));
}

// Plan task 5: the flyout must render through the composition surface
// (GDI content over acrylic is additive and black-keyed, so GDI is only
// the no-composition fallback), and the material must follow the system
// settings rather than always painting an opaque fill.
TEST(FlyoutIntegration, UsesTheCompositionSurfaceAndReportsItsMaterial) {
  const MonitorTaskbar mt = SyntheticTaskbar();
  auto flyout = Flyout::Create(mt.monitor, ComputeOverlayStripRect(mt, 200),
                               {FirstIcon(MakeTarget(reinterpret_cast<HWND>(0xA1)))},
                               [](const DispatchCall&) {}, []() {});
  ASSERT_NE(flyout, nullptr);
  EXPECT_TRUE(flyout->usesComposition());
  EXPECT_EQ(flyout->materialActive(),
            traymirror::SystemTransparencyEnabled() &&
                !traymirror::SystemUsesHighContrast());
  SendMessageW(flyout->hwnd(), WM_KEYDOWN, VK_ESCAPE, 0);
  FinishTransition(flyout->hwnd());
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
  FinishTransition(hwnd);

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
  FinishTransition(hwnd);

  EXPECT_TRUE(closed);
  EXPECT_EQ(IsWindow(hwnd), FALSE);
}

// Plan task 6: the popup fades in from transparent, driven by its own
// timer, so no message handler blocks for the animation.
TEST(FlyoutIntegration, OpeningFadesInAndReachesFullOpacity) {
  if (!AnimationsEnabled()) {
    GTEST_SKIP() << "system animations are disabled";
  }
  const MonitorTaskbar mt = SyntheticTaskbar();
  auto flyout = Flyout::Create(mt.monitor, ComputeOverlayStripRect(mt, 200),
                               {FirstIcon(MakeTarget(reinterpret_cast<HWND>(0xA1)))},
                               [](const DispatchCall&) {}, []() {});
  ASSERT_NE(flyout, nullptr);
  const HWND hwnd = flyout->hwnd();
  EXPECT_EQ(LayeredAlpha(hwnd), 0);

  Sleep(PopupTransition::kDefaultDurationMs + 60);
  SendMessageW(hwnd, WM_TIMER, 0, 0);
  EXPECT_EQ(LayeredAlpha(hwnd), 255);

  SendMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
  FinishTransition(hwnd);
}

TEST(FlyoutIntegration, ClosingFadesBeforeDestroyingTheWindow) {
  if (!AnimationsEnabled()) {
    GTEST_SKIP() << "system animations are disabled";
  }
  const MonitorTaskbar mt = SyntheticTaskbar();
  bool closed = false;
  auto flyout = Flyout::Create(mt.monitor, ComputeOverlayStripRect(mt, 200),
                               {FirstIcon(MakeTarget(reinterpret_cast<HWND>(0xA1)))},
                               [](const DispatchCall&) {},
                               [&]() { closed = true; });
  ASSERT_NE(flyout, nullptr);
  const HWND hwnd = flyout->hwnd();
  FinishTransition(hwnd);  // opening completed
  ASSERT_EQ(LayeredAlpha(hwnd), 255);

  flyout->Close();
  EXPECT_TRUE(flyout->IsClosing());
  EXPECT_TRUE(IsWindow(hwnd));  // still displayed while fading
  EXPECT_EQ(LayeredAlpha(hwnd), 255);

  Sleep(16);
  SendMessageW(hwnd, WM_TIMER, 0, 0);
  EXPECT_TRUE(IsWindow(hwnd));
  const int mid = LayeredAlpha(hwnd);
  EXPECT_GT(mid, 0);
  EXPECT_LT(mid, 255);

  FinishTransition(hwnd);
  EXPECT_FALSE(IsWindow(hwnd));
  EXPECT_TRUE(closed);  // the owner hears only after the fade completed
}

TEST(FlyoutIntegration, ReopeningDuringCloseKeepsThePopupAlive) {
  if (!AnimationsEnabled()) {
    GTEST_SKIP() << "system animations are disabled";
  }
  const MonitorTaskbar mt = SyntheticTaskbar();
  auto flyout = Flyout::Create(mt.monitor, ComputeOverlayStripRect(mt, 200),
                               {FirstIcon(MakeTarget(reinterpret_cast<HWND>(0xA1)))},
                               [](const DispatchCall&) {}, []() {});
  ASSERT_NE(flyout, nullptr);
  const HWND hwnd = flyout->hwnd();
  FinishTransition(hwnd);
  ASSERT_EQ(LayeredAlpha(hwnd), 255);

  flyout->Close();
  Sleep(16);
  SendMessageW(hwnd, WM_TIMER, 0, 0);
  const int mid = LayeredAlpha(hwnd);
  ASSERT_GT(mid, 0);
  ASSERT_LT(mid, 255);

  EXPECT_TRUE(flyout->Reopen());
  EXPECT_FALSE(flyout->IsClosing());
  FinishTransition(hwnd);
  EXPECT_TRUE(IsWindow(hwnd));
  EXPECT_EQ(LayeredAlpha(hwnd), 255);

  flyout->CloseImmediately();
  EXPECT_FALSE(IsWindow(hwnd));
}

TEST(FlyoutIntegration, ClosingPopupDoesNotDispatchClicks) {
  if (!AnimationsEnabled()) {
    GTEST_SKIP() << "system animations are disabled";
  }
  const MonitorTaskbar mt = SyntheticTaskbar();
  std::vector<DispatchCall> calls;
  auto flyout = Flyout::Create(
      mt.monitor, ComputeOverlayStripRect(mt, 200),
      {FirstIcon(MakeTarget(reinterpret_cast<HWND>(0xA1))),
       FirstIcon(MakeTarget(reinterpret_cast<HWND>(0xB2)))},
      [&](const DispatchCall& call) { calls.push_back(call); }, []() {});
  ASSERT_NE(flyout, nullptr);
  const HWND hwnd = flyout->hwnd();
  FinishTransition(hwnd);
  ASSERT_EQ(LayeredAlpha(hwnd), 255);

  flyout->Close();
  Sleep(16);
  SendMessageW(hwnd, WM_TIMER, 0, 0);
  ASSERT_LT(LayeredAlpha(hwnd), 255);

  const GridLayout grid = LayoutGrid(IconPxForDpi(96), 2);
  const POINT pt = {(grid.cells[0].left + grid.cells[0].right) / 2,
                    (grid.cells[0].top + grid.cells[0].bottom) / 2};
  SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(pt.x, pt.y));
  SendMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(pt.x, pt.y));

  EXPECT_TRUE(calls.empty());  // the fade is not a target
  EXPECT_TRUE(IsWindow(hwnd));
  FinishTransition(hwnd);
  EXPECT_FALSE(IsWindow(hwnd));
}

TEST(FlyoutIntegration, RapidChevronClicksReverseTheSameFlyout) {
  if (!AnimationsEnabled()) {
    GTEST_SKIP() << "system animations are disabled";
  }
  const MonitorTaskbar mt = SyntheticTaskbar();
  const RECT strip = ComputeOverlayStripRect(mt, 400);
  auto overlay = Overlay::Create(mt.monitor, strip, {},
                                 [](const DispatchCall&) {});
  ASSERT_NE(overlay, nullptr);
  overlay->SetIcons({FirstIcon(MakeTarget(reinterpret_cast<HWND>(0x1)))},
                    {FirstIcon(MakeTarget(reinterpret_cast<HWND>(0x2)))});
  overlay->Render();
  const POINT chevron = ChevronPoint(strip, GetDpiForWindow(overlay->hwnd()));

  SendMessageW(overlay->hwnd(), WM_LBUTTONUP, 0,
               MAKELPARAM(chevron.x, chevron.y));
  const HWND flyout = FirstWindowOfClass(kFlyoutClass);
  ASSERT_NE(flyout, nullptr);
  Sleep(20);
  SendMessageW(flyout, WM_TIMER, 0, 0);  // partially open
  const int mid = LayeredAlpha(flyout);
  ASSERT_GT(mid, 0);
  ASSERT_LT(mid, 255);

  // A second click starts the close; the third reverses it before any
  // further tick can complete the fade. No sleep between them, so the
  // outcome is deterministic on a loaded machine too.
  SendMessageW(overlay->hwnd(), WM_LBUTTONUP, 0,
               MAKELPARAM(chevron.x, chevron.y));  // closing
  EXPECT_TRUE(IsWindow(flyout));
  EXPECT_EQ(LayeredAlpha(flyout), mid);  // no jump at the reversal point

  SendMessageW(overlay->hwnd(), WM_LBUTTONUP, 0,
               MAKELPARAM(chevron.x, chevron.y));  // reopen
  FinishTransition(flyout);

  EXPECT_TRUE(IsWindow(flyout));
  EXPECT_EQ(LayeredAlpha(flyout), 255);
  EXPECT_EQ(FirstWindowOfClass(kFlyoutClass), flyout);  // no duplicate
  EXPECT_NE(IsWindowVisible(overlay->hwnd()), FALSE);

  SendMessageW(flyout, WM_KEYDOWN, VK_ESCAPE, 0);
  FinishTransition(flyout);
}

TEST(FlyoutIntegration, StaleCloseDoesNotDestroyAReopenedFlyout) {
  const MonitorTaskbar mt = SyntheticTaskbar();
  const RECT strip = ComputeOverlayStripRect(mt, 400);
  auto overlay = Overlay::Create(mt.monitor, strip, {},
                                 [](const DispatchCall&) {});
  ASSERT_NE(overlay, nullptr);
  overlay->SetIcons({FirstIcon(MakeTarget(reinterpret_cast<HWND>(0x1)))},
                    {FirstIcon(MakeTarget(reinterpret_cast<HWND>(0x2)))});
  overlay->Render();
  const POINT chevron = ChevronPoint(strip, GetDpiForWindow(overlay->hwnd()));

  SendMessageW(overlay->hwnd(), WM_LBUTTONUP, 0,
               MAKELPARAM(chevron.x, chevron.y));
  HWND first = FirstWindowOfClass(kFlyoutClass);
  ASSERT_NE(first, nullptr);
  SendMessageW(first, WM_KEYDOWN, VK_ESCAPE, 0);
  FinishTransition(first);
  EXPECT_EQ(IsWindow(first), FALSE);
  // Its close notification is now posted but not yet handled.

  SendMessageW(overlay->hwnd(), WM_LBUTTONUP, 0,
               MAKELPARAM(chevron.x, chevron.y));
  const HWND second = FirstWindowOfClass(kFlyoutClass);
  ASSERT_NE(second, nullptr);
  ASSERT_NE(second, first);

  MSG msg = {};
  while (PeekMessageW(&msg, overlay->hwnd(), 0, 0, PM_REMOVE) != 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  EXPECT_TRUE(IsWindow(second));
  EXPECT_NE(IsWindowVisible(second), FALSE);

  SendMessageW(second, WM_KEYDOWN, VK_ESCAPE, 0);
  FinishTransition(second);
}

TEST(FlyoutIntegration, IconUpdatesDoNotCancelTheOpenTransition) {
  if (!AnimationsEnabled()) {
    GTEST_SKIP() << "system animations are disabled";
  }
  const MonitorTaskbar mt = SyntheticTaskbar();
  const RECT strip = ComputeOverlayStripRect(mt, 400);
  auto overlay = Overlay::Create(mt.monitor, strip, {},
                                 [](const DispatchCall&) {});
  ASSERT_NE(overlay, nullptr);
  overlay->SetIcons({FirstIcon(MakeTarget(reinterpret_cast<HWND>(0x1)))},
                    {FirstIcon(MakeTarget(reinterpret_cast<HWND>(0x2)))});
  overlay->Render();
  const POINT chevron = ChevronPoint(strip, GetDpiForWindow(overlay->hwnd()));
  SendMessageW(overlay->hwnd(), WM_LBUTTONUP, 0,
               MAKELPARAM(chevron.x, chevron.y));
  const HWND flyout = FirstWindowOfClass(kFlyoutClass);
  ASSERT_NE(flyout, nullptr);

  // An animated icon update while the popup is still opening.
  overlay->SetIcons({}, {FirstIcon(MakeTarget(reinterpret_cast<HWND>(0x2)),
                                  L"updated")});
  overlay->Render();
  EXPECT_TRUE(IsWindow(flyout));

  FinishTransition(flyout);
  EXPECT_TRUE(IsWindow(flyout));
  EXPECT_EQ(LayeredAlpha(flyout), 255);

  SendMessageW(flyout, WM_KEYDOWN, VK_ESCAPE, 0);
  FinishTransition(flyout);
}

// The tray tooltip appears after the hover delay, above the hovered cell.
TEST(FlyoutInteraction, HoverShowsTheIconTooltip) {
  const MonitorTaskbar mt = SyntheticTaskbar();
  const RECT strip = ComputeOverlayStripRect(mt, 200);
  auto flyout = Flyout::Create(mt.monitor, strip,
                               {FirstIcon(MakeTarget(reinterpret_cast<HWND>(0xA1)),
                                          L"hidden tip")},
                               [](const DispatchCall&) {}, []() {});
  ASSERT_NE(flyout, nullptr);

  SendMessageW(flyout->hwnd(), WM_MOUSEMOVE, 0, MAKELPARAM(28, 28));
  SendMessageW(flyout->hwnd(), WM_MOUSEHOVER, 0, 0);

  HWND tip = VisibleTooltip();
  ASSERT_NE(tip, nullptr);
  EXPECT_EQ(TooltipText(tip), L"hidden tip");
  RECT tipRect = {};
  RECT flyRect = {};
  ASSERT_NE(GetWindowRect(tip, &tipRect), FALSE);
  ASSERT_NE(GetWindowRect(flyout->hwnd(), &flyRect), FALSE);
  EXPECT_LE(tipRect.bottom, flyRect.top);  // above the popup, never over it

  SendMessageW(flyout->hwnd(), WM_MOUSELEAVE, 0, 0);
  EXPECT_EQ(VisibleTooltip(), nullptr);
  flyout->CloseImmediately();
}

TEST(FlyoutInteraction, TooltipFollowsTheHoveredCell) {
  const MonitorTaskbar mt = SyntheticTaskbar();
  const RECT strip = ComputeOverlayStripRect(mt, 200);
  auto flyout = Flyout::Create(
      mt.monitor, strip,
      {FirstIcon(MakeTarget(reinterpret_cast<HWND>(0xA1)), L"first"),
       FirstIcon(MakeTarget(reinterpret_cast<HWND>(0xA2)), L"second")},
      [](const DispatchCall&) {}, []() {});
  ASSERT_NE(flyout, nullptr);

  SendMessageW(flyout->hwnd(), WM_MOUSEMOVE, 0, MAKELPARAM(28, 28));
  SendMessageW(flyout->hwnd(), WM_MOUSEHOVER, 0, 0);
  HWND tip = VisibleTooltip();
  ASSERT_NE(tip, nullptr);
  EXPECT_EQ(TooltipText(tip), L"first");

  SendMessageW(flyout->hwnd(), WM_MOUSEMOVE, 0, MAKELPARAM(68, 28));
  EXPECT_EQ(VisibleTooltip(), nullptr);  // the old cell's tip is gone
  SendMessageW(flyout->hwnd(), WM_MOUSEHOVER, 0, 0);
  tip = VisibleTooltip();
  ASSERT_NE(tip, nullptr);
  EXPECT_EQ(TooltipText(tip), L"second");
  flyout->CloseImmediately();
}

// A model update under a stationary pointer refreshes the visible tip.
TEST(FlyoutInteraction, TooltipUpdatesUnderAStationaryPointer) {
  const MonitorTaskbar mt = SyntheticTaskbar();
  const RECT strip = ComputeOverlayStripRect(mt, 200);
  auto flyout = Flyout::Create(
      mt.monitor, strip,
      {FirstIcon(MakeTarget(reinterpret_cast<HWND>(0xA1)), L"before")},
      [](const DispatchCall&) {}, []() {});
  ASSERT_NE(flyout, nullptr);
  SendMessageW(flyout->hwnd(), WM_MOUSEMOVE, 0, MAKELPARAM(28, 28));
  SendMessageW(flyout->hwnd(), WM_MOUSEHOVER, 0, 0);
  HWND tip = VisibleTooltip();
  ASSERT_NE(tip, nullptr);
  ASSERT_EQ(TooltipText(tip), L"before");

  flyout->Update(
      {FirstIcon(MakeTarget(reinterpret_cast<HWND>(0xA1)), L"after")}, strip);
  tip = VisibleTooltip();
  ASSERT_NE(tip, nullptr);
  EXPECT_EQ(TooltipText(tip), L"after");
  flyout->CloseImmediately();
}

TEST(FlyoutInteraction, RightClickDispatchesTheOwnerMenuAndCloses) {
  const MonitorTaskbar mt = SyntheticTaskbar();
  const RECT strip = ComputeOverlayStripRect(mt, 200);
  std::vector<DispatchCall> calls;
  auto flyout = Flyout::Create(
      mt.monitor, strip,
      {FirstIcon(MakeTarget(reinterpret_cast<HWND>(0xA1)))},
      [&calls](const DispatchCall& call) { calls.push_back(call); }, []() {});
  ASSERT_NE(flyout, nullptr);
  const HWND hwnd = flyout->hwnd();

  SendMessageW(hwnd, WM_RBUTTONDOWN, MK_RBUTTON, MAKELPARAM(28, 28));
  SendMessageW(hwnd, WM_RBUTTONUP, 0, MAKELPARAM(28, 28));

  ASSERT_EQ(calls.size(), 2U);
  EXPECT_EQ(calls[0].event, traymirror::TrayEvent::kRightDown);
  EXPECT_EQ(calls[1].event, traymirror::TrayEvent::kRightUp);
  EXPECT_EQ(calls[0].target.owner, reinterpret_cast<HWND>(0xA1));
  FinishTransition(hwnd);
  EXPECT_EQ(FirstWindowOfClass(kFlyoutClass), nullptr);
}

TEST(InteractionCursors, StripAndOverflowUseTheArrowCursor) {
  const MonitorTaskbar mt = SyntheticTaskbar();
  const RECT strip = ComputeOverlayStripRect(mt, 400);
  auto overlay = Overlay::Create(mt.monitor, strip, {},
                                 [](const DispatchCall&) {});
  ASSERT_NE(overlay, nullptr);
  ExpectArrowCursor(overlay->hwnd());

  overlay->SetIcons({FirstIcon(MakeTarget(reinterpret_cast<HWND>(0x1)))},
                    {FirstIcon(MakeTarget(reinterpret_cast<HWND>(0x2)))});
  overlay->Render();
  const POINT chevron = ChevronPoint(strip, GetDpiForWindow(overlay->hwnd()));
  SendMessageW(overlay->hwnd(), WM_LBUTTONUP, 0,
               MAKELPARAM(chevron.x, chevron.y));
  const HWND flyout = FirstWindowOfClass(kFlyoutClass);
  ASSERT_NE(flyout, nullptr);
  ExpectArrowCursor(flyout);
  SendMessageW(flyout, WM_KEYDOWN, VK_ESCAPE, 0);
  FinishTransition(flyout);
}

// The strip's tooltip waits for the system hover time, follows the model
// under a stationary pointer and disappears when the pointer leaves.
TEST(OverlayInteraction, TooltipWaitsUpdatesAndLeaves) {
  const MonitorTaskbar mt = SyntheticTaskbar();
  const RECT strip = ComputeOverlayStripRect(mt, 200);
  auto overlay = Overlay::Create(mt.monitor, strip, {},
                                 [](const DispatchCall&) {});
  ASSERT_NE(overlay, nullptr);
  const DispatchTarget target = MakeTarget(reinterpret_cast<HWND>(0xB1));
  overlay->SetIcons({FirstIcon(target, L"before")});
  overlay->Render();

  SendMessageW(overlay->hwnd(), WM_MOUSEMOVE, 0, MAKELPARAM(16, 24));
  EXPECT_EQ(VisibleTooltip(), nullptr);  // not before the hover delay
  SendMessageW(overlay->hwnd(), WM_MOUSEHOVER, 0, 0);
  HWND tip = VisibleTooltip();
  ASSERT_NE(tip, nullptr);
  EXPECT_EQ(TooltipText(tip), L"before");

  overlay->SetIcons({FirstIcon(target, L"after")});
  overlay->Render();
  tip = VisibleTooltip();
  ASSERT_NE(tip, nullptr);
  EXPECT_EQ(TooltipText(tip), L"after");

  SendMessageW(overlay->hwnd(), WM_MOUSELEAVE, 0, 0);
  EXPECT_EQ(VisibleTooltip(), nullptr);
}

// Esc pressed twice must not blank the popup for one timer tick; the
// transition keeps the displayed alpha when close is requested again.
TEST(FlyoutIntegration, DoubleEscapeDoesNotBlankTheFadingPopup) {
  if (!AnimationsEnabled()) {
    GTEST_SKIP() << "system animations are disabled";
  }
  const MonitorTaskbar mt = SyntheticTaskbar();
  const RECT strip = ComputeOverlayStripRect(mt, 200);
  auto flyout = Flyout::Create(
      mt.monitor, strip,
      {FirstIcon(MakeTarget(reinterpret_cast<HWND>(0xA1)))},
      [](const DispatchCall&) {}, []() {});
  ASSERT_NE(flyout, nullptr);
  const HWND hwnd = flyout->hwnd();
  FinishTransition(hwnd);
  ASSERT_EQ(LayeredAlpha(hwnd), 255);

  SendMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
  SendMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
  EXPECT_GT(LayeredAlpha(hwnd), 0);  // no transparent flash

  FinishTransition(hwnd);
  EXPECT_EQ(IsWindow(hwnd), FALSE);
}
