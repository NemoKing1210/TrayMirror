// Pure geometry tests for the overlay strip (spec 6.1). No desktop
// access: every value is synthetic.
#include "win/Monitors.h"

#include <gtest/gtest.h>

namespace {

using traymirror::ComputeOverlayStripRect;
using traymirror::IconPxForDpi;
using traymirror::MonitorTaskbar;

MonitorTaskbar MakeTaskbar() {
  MonitorTaskbar mt;
  mt.monitorRect = {0, 0, 1920, 1080};
  mt.taskbarRect = {0, 1000, 1920, 1080};
  mt.clockRect = {1800, 1000, 1920, 1080};
  mt.hasTaskbar = true;
  mt.hasTaskListBounds = true;
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

TEST(MonitorGeometry, StripNeverCoversTaskList) {
  // The task list has grown left-to-right into the space the strip
  // would occupy: the strip must start at the task list's right edge,
  // or its topmost window would swallow clicks meant for task buttons.
  MonitorTaskbar mt = MakeTaskbar();
  mt.taskListRight = 1700;
  const RECT strip = ComputeOverlayStripRect(mt, 200);
  EXPECT_EQ(strip.left, 1700);
  EXPECT_EQ(strip.right, 1800);
}

TEST(MonitorGeometry, TaskListReachingTheClockLeavesNoStrip) {
  MonitorTaskbar mt = MakeTaskbar();
  mt.taskListRight = 1800;
  const RECT strip = ComputeOverlayStripRect(mt, 200);
  EXPECT_EQ(strip.right, 0);  // all-zero: the caller creates no overlay
  EXPECT_EQ(strip.left, 0);
}

TEST(MonitorGeometry, UnknownTaskListDoesNotCoverUnmeasuredApplications) {
  MonitorTaskbar mt = MakeTaskbar();
  mt.hasTaskListBounds = false;
  const RECT strip = ComputeOverlayStripRect(mt, 200);
  EXPECT_EQ(strip.left, 0);
  EXPECT_EQ(strip.right, 0);
}

TEST(MonitorGeometry, ApplicationsOnALeftHandMonitorAreProtected) {
  MonitorTaskbar mt = MakeTaskbar();
  mt.monitorRect = {-1920, 0, 0, 1080};
  mt.taskbarRect = {-1920, 1000, 0, 1080};
  mt.clockRect = {-120, 1000, 0, 1080};
  mt.taskListRight = -200;
  const RECT strip = ComputeOverlayStripRect(mt, 400);
  EXPECT_EQ(strip.left, -200);
  EXPECT_EQ(strip.right, -120);
}

// The icon is a system small icon, so the size is the system metric at
// that DPI: it scales linearly (16 px at 96, 40 px at 240). Stage 2
// capped it at 32 px, which was an assumption, not a measurement.
TEST(MonitorGeometry, IconPxByDpi) {
  EXPECT_EQ(IconPxForDpi(96), 16);
  EXPECT_EQ(IconPxForDpi(120), 20);
  EXPECT_EQ(IconPxForDpi(144), 24);
  EXPECT_EQ(IconPxForDpi(192), 32);
  EXPECT_EQ(IconPxForDpi(240), 40);
  EXPECT_EQ(IconPxForDpi(0), 16);  // an unknown DPI behaves as 96
}

TEST(MonitorGeometry, NativeButtonBoundsScaleFromTheirOwnMonitorDpi) {
  const traymirror::TrayMetrics reference{48, 60, 144};
  const SIZE normal = traymirror::TrayButtonSizeForDpi(96, reference);
  EXPECT_EQ(normal.cx, 32);
  EXPECT_EQ(normal.cy, 40);
  const SIZE doubled = traymirror::TrayButtonSizeForDpi(192, reference);
  EXPECT_EQ(doubled.cx, 64);
  EXPECT_EQ(doubled.cy, 80);
}

TEST(MonitorGeometry, InvalidNativeBoundsUseTheScalableFallback) {
  const SIZE button = traymirror::TrayButtonSizeForDpi(144, {0, 0, 0});
  EXPECT_EQ(button.cx, 48);
  EXPECT_EQ(button.cy, 72);
}

TEST(MonitorGeometry, ApplicationButtonsGrowingShrinkTheStrip) {
  MonitorTaskbar mt = MakeTaskbar();
  mt.taskListRight = 1500;
  EXPECT_EQ(ComputeOverlayStripRect(mt, 400).left, 1500);
  mt.taskListRight = 1700;  // more buttons: less free space
  EXPECT_EQ(ComputeOverlayStripRect(mt, 400).left, 1700);
  mt.taskListRight = 1800;  // reaches the clock: zero slots, no strip
  const RECT none = ComputeOverlayStripRect(mt, 400);
  EXPECT_EQ(none.left, 0);
  EXPECT_EQ(none.right, 0);
}

TEST(MonitorGeometry, TaskbarAboveThePrimaryKeepsNegativeCoordinates) {
  MonitorTaskbar mt = MakeTaskbar();
  mt.monitorRect = {0, -1080, 1920, 0};
  mt.taskbarRect = {0, -48, 1920, 0};
  mt.clockRect = {1800, -48, 1920, 0};
  mt.taskListRight = 1600;
  const RECT strip = ComputeOverlayStripRect(mt, 200);
  EXPECT_EQ(strip.left, 1600);
  EXPECT_EQ(strip.right, 1800);
  EXPECT_EQ(strip.top, -48);
  EXPECT_EQ(strip.bottom, 0);
}

TEST(MonitorGeometry, FullTaskbarHidesTheStripInsteadOfCoveringButtons) {
  MonitorTaskbar mt = MakeTaskbar();
  mt.taskListRight = mt.clockRect.left;  // every slot is occupied
  const RECT strip = ComputeOverlayStripRect(mt, 32);
  EXPECT_EQ(strip.right, 0);
  EXPECT_EQ(strip.left, 0);
  // The caller keeps the window but marks the geometry unsafe; the
  // zero-slot state is a geometry outcome, not a desktop click.
}

TEST(MonitorGeometry, FullscreenSuppressionIsLocalToItsMonitor) {
  const RECT monitor{0, 0, 1920, 1080};
  EXPECT_FALSE(traymirror::ShouldExposeTray(monitor, RECT{0, 0, 1920, 1080}));
  EXPECT_TRUE(traymirror::ShouldExposeTray(monitor, RECT{0, 0, 1920, 1040}));
  EXPECT_TRUE(traymirror::ShouldExposeTray(monitor, RECT{1920, 0, 3840, 1080}));
}

TEST(MonitorGeometry, DesktopCoverageDoesNotSuppressTheTray) {
  const RECT monitor{0, 0, 1920, 1080};
  EXPECT_TRUE(traymirror::ShouldExposeTray(monitor, monitor, true));
}

}  // namespace
