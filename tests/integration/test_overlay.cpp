// Overlay smoke tests (spec 6.1/6.2): real windows and a real
// D2D/DComp surface, synthetic geometry — no Explorer contact. A
// composition failure must degrade (window alive, no render), never
// crash, so the assertions are about the window, not the GPU.
#include "render/Overlay.h"
#include "render/OverlayThread.h"

#include "core/ModelInbox.h"
#include "core/TrayModel.h"
#include "win/Monitors.h"

#include <windows.h>

#include <commctrl.h>

#include <gtest/gtest.h>

#include <cwchar>
#include <iterator>
#include <memory>
#include <vector>

namespace {

using traymirror::ComputeOverlayStripRect;
using traymirror::ModelInbox;
using traymirror::MonitorTaskbar;
using traymirror::Overlay;
using traymirror::OverlayThread;
using traymirror::TrayModel;

// The system tooltip window is a top-level window of class
// tooltips_class32 owned by the overlay. Found by enumeration so the
// test never depends on the tooltip's own internals.
HWND g_tooltipOwner = nullptr;
HWND g_tooltipFound = nullptr;

BOOL CALLBACK FindTooltipProc(HWND hwnd, LPARAM /*unused*/) {
  wchar_t cls[64] = {};
  if (GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls))) == 0 ||
      wcscmp(cls, L"tooltips_class32") != 0) {
    return TRUE;
  }
  if (GetWindow(hwnd, GW_OWNER) != g_tooltipOwner) {
    return TRUE;
  }
  g_tooltipFound = hwnd;
  return FALSE;
}

HWND FindOwnedTooltip(HWND owner) {
  g_tooltipOwner = owner;
  g_tooltipFound = nullptr;
  EnumWindows(FindTooltipProc, 0);
  return g_tooltipFound;
}

// A taskbar row on the monitor the test process starts on: the real
// work-area edge, a measured-width clock at its right edge.
MonitorTaskbar SyntheticTaskbar() {
  const POINT origin = {0, 0};
  const HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO mi = {};
  mi.cbSize = sizeof(mi);
  const BOOL ok = GetMonitorInfoW(monitor, &mi);
  EXPECT_NE(ok, FALSE);

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

}  // namespace

TEST(OverlaySmoke, CreatesAndDestroysStrip) {
  const MonitorTaskbar mt = SyntheticTaskbar();
  const RECT strip = ComputeOverlayStripRect(mt, 200);
  auto overlay = Overlay::Create(mt.monitor, strip);
  ASSERT_NE(overlay, nullptr);
  EXPECT_NE(overlay->hwnd(), nullptr);
  overlay->SetIcons({});
  overlay->Render();
  overlay.reset();  // destruction must not hang or crash
}

TEST(OverlaySmoke, StripMatchesGeometry) {
  const MonitorTaskbar mt = SyntheticTaskbar();
  const RECT strip = ComputeOverlayStripRect(mt, 200);
  auto overlay = Overlay::Create(mt.monitor, strip);
  ASSERT_NE(overlay, nullptr);

  RECT actual = {};
  ASSERT_NE(GetWindowRect(overlay->hwnd(), &actual), FALSE);
  EXPECT_EQ(actual.left, strip.left);
  EXPECT_EQ(actual.top, strip.top);
  EXPECT_EQ(actual.right, strip.right);
  EXPECT_EQ(actual.bottom, strip.bottom);
}

TEST(OverlaySmoke, DrawsIconsWithRealD2D) {
  const MonitorTaskbar mt = SyntheticTaskbar();
  const RECT strip = ComputeOverlayStripRect(mt, 200);
  auto overlay = Overlay::Create(mt.monitor, strip);
  ASSERT_NE(overlay, nullptr);

  // Real icons plus one null handle (the placeholder path). A device
  // failure inside drawing must degrade to a log line, never a crash.
  overlay->SetIcons(
      {traymirror::IconView{.icon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512)),
                            .serial = 1,
                            .tip = L"first"},
       traymirror::IconView{.icon = LoadIconW(nullptr, MAKEINTRESOURCEW(32513)),
                            .serial = 2,
                            .tip = L"second"},
       traymirror::IconView{.icon = nullptr, .serial = 3, .tip = L"missing"}});
  overlay->Render();
  overlay->Render();  // clean: no dirty flag, no redraw
}

TEST(OverlaySmoke, TooltipShowsAndHides) {
  const MonitorTaskbar mt = SyntheticTaskbar();
  const RECT strip = ComputeOverlayStripRect(mt, 200);
  auto overlay = Overlay::Create(mt.monitor, strip);
  ASSERT_NE(overlay, nullptr);
  overlay->SetIcons({traymirror::IconView{
      .icon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512)),
      .serial = 1,
      .tip = L"first tip"}});
  overlay->Render();

  // The first cell starts at the strip's left edge; the icon is
  // vertically centered, so the cell's middle is the strip's middle.
  const int height = strip.bottom - strip.top;
  const POINT inside = {4, height / 2};
  SendMessageW(overlay->hwnd(), WM_MOUSEMOVE, 0,
               MAKELPARAM(inside.x, inside.y));

  const HWND tip = FindOwnedTooltip(overlay->hwnd());
  ASSERT_NE(tip, nullptr);
  EXPECT_NE(IsWindowVisible(tip), FALSE);
  wchar_t text[128] = {};
  TOOLINFOW info = {};
  info.cbSize = sizeof(info);
  info.hwnd = overlay->hwnd();
  info.uId = reinterpret_cast<UINT_PTR>(overlay->hwnd());
  info.lpszText = text;
  SendMessageW(tip, TTM_GETTEXTW, 128, reinterpret_cast<LPARAM>(&info));
  EXPECT_STREQ(text, L"first tip");

  SendMessageW(overlay->hwnd(), WM_MOUSELEAVE, 0, 0);
  EXPECT_EQ(IsWindowVisible(tip), FALSE);
}

TEST(OverlaySmoke, UiWindowExistsWhileRunning) {  auto enumerate = []() {
    return std::vector<MonitorTaskbar>{SyntheticTaskbar()};
  };
  ModelInbox inbox;
  TrayModel model;
  OverlayThread thread;
  ASSERT_TRUE(thread.Start(&inbox, &model, enumerate));

  const HWND ui = thread.uiHwnd();
  ASSERT_NE(ui, nullptr);
  EXPECT_NE(IsWindow(ui), FALSE);

  thread.Stop();
  EXPECT_EQ(IsWindow(ui), FALSE);
}
