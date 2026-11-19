// Monitor and secondary-taskbar geometry (spec 6.1): where each
// secondary taskbar sits and where the overlay strip goes. The pure
// functions are unit-tested; only DefaultEnumerateSecondaryMonitors
// touches the desktop.
#pragma once

#include <windows.h>

#include <functional>
#include <vector>

namespace traymirror {

struct MonitorTaskbar {
  HMONITOR monitor = nullptr;
  RECT monitorRect = {};
  RECT taskbarRect = {};
  // The taskbar clock's rect. Measured on Win11 build 26300: the clock
  // is not a child HWND (TrayClockWClass is absent from both taskbars),
  // so this comes from UI Automation. When UIA yields nothing the
  // enumeration reserves measured space at the right edge instead, so
  // the strip still cannot cover the clock.
  RECT clockRect = {};
  // The task list's right edge (UIA, MSTaskListWClass), or 0 when it
  // cannot be measured. The strip must never start left of it: the
  // overlay is topmost, so anything it covers is unclickable — task
  // buttons included. The gap between this edge and the clock shrinks
  // as the user opens windows.
  LONG taskListRight = 0;
  bool hasTaskbar = false;
};

using MonitorEnumerator = std::function<std::vector<MonitorTaskbar>()>;

// Secondary monitors only, in EnumDisplayMonitors order.
std::vector<MonitorTaskbar> DefaultEnumerateSecondaryMonitors();

// Pure: the taskbar row of `mt` ending at the clock's left edge, never
// leaving the monitor and never covering the task list. No taskbar, a
// non-positive width, or a task list that reaches the clock gives an
// all-zero RECT.
RECT ComputeOverlayStripRect(const MonitorTaskbar& mt, int widthPx);

// Icon metrics per DPI (plan ruling R7).
int IconPxForDpi(UINT dpi);
int IconGapPxForDpi(UINT dpi);

}  // namespace traymirror
