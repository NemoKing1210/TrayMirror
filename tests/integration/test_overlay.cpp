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

#include <gtest/gtest.h>

#include <memory>
#include <vector>

namespace {

using traymirror::ComputeOverlayStripRect;
using traymirror::ModelInbox;
using traymirror::MonitorTaskbar;
using traymirror::Overlay;
using traymirror::OverlayThread;
using traymirror::TrayModel;

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

TEST(OverlaySmoke, UiWindowExistsWhileRunning) {
  auto enumerate = []() {
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
