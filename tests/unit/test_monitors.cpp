// Pure geometry tests for the overlay strip (spec 6.1). No desktop
// access: every value is synthetic.
#include "win/Monitors.h"

#include <gtest/gtest.h>

namespace {

using traymirror::ComputeOverlayStripRect;
using traymirror::IconGapPxForDpi;
using traymirror::IconPxForDpi;
using traymirror::MonitorTaskbar;

MonitorTaskbar MakeTaskbar() {
  MonitorTaskbar mt;
  mt.monitorRect = {0, 0, 1920, 1080};
  mt.taskbarRect = {0, 1000, 1920, 1080};
  mt.clockRect = {1800, 1000, 1920, 1080};
  mt.hasTaskbar = true;
  return mt;
}

TEST(MonitorGeometry, StripSitsLeftOfClock) {
  const RECT strip = ComputeOverlayStripRect(MakeTaskbar(), 200);
  EXPECT_EQ(strip.right, 1800);
  EXPECT_EQ(strip.left, 1600);
  EXPECT_EQ(strip.top, 1000);
  EXPECT_EQ(strip.bottom, 1080);
}

TEST(MonitorGeometry, StripClampedToMonitor) {
  // The strip is wider than the space left of the clock: the left edge
  // stops at the monitor's left edge instead of going off-screen.
  const RECT strip = ComputeOverlayStripRect(MakeTaskbar(), 2000);
  EXPECT_EQ(strip.left, 0);
  EXPECT_EQ(strip.right, 1800);
}

TEST(MonitorGeometry, NoTaskbarYieldsEmptyRect) {
  MonitorTaskbar mt = MakeTaskbar();
  mt.hasTaskbar = false;
  const RECT strip = ComputeOverlayStripRect(mt, 200);
  EXPECT_EQ(strip.left, 0);
  EXPECT_EQ(strip.top, 0);
  EXPECT_EQ(strip.right, 0);
  EXPECT_EQ(strip.bottom, 0);
}

TEST(MonitorGeometry, IconPxByDpi) {
  EXPECT_EQ(IconPxForDpi(96), 16);
  EXPECT_EQ(IconPxForDpi(120), 20);
  EXPECT_EQ(IconPxForDpi(144), 24);
  EXPECT_EQ(IconPxForDpi(192), 32);
  EXPECT_EQ(IconPxForDpi(240), 32);
}

TEST(MonitorGeometry, GapScalesWithDpi) {
  EXPECT_EQ(IconGapPxForDpi(96), 2);
  EXPECT_EQ(IconGapPxForDpi(192), 4);
}

}  // namespace
