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
  bool hasTaskbar = false;
};

using MonitorEnumerator = std::function<std::vector<MonitorTaskbar>()>;

// Secondary monitors only, in EnumDisplayMonitors order.
std::vector<MonitorTaskbar> DefaultEnumerateSecondaryMonitors();

// Pure: the taskbar row of `mt` ending at the clock's left edge and
// never leaving the monitor. No taskbar (or a non-positive width) gives
// an all-zero RECT.
RECT ComputeOverlayStripRect(const MonitorTaskbar& mt, int widthPx);

// Icon metrics per DPI (plan ruling R7).
int IconPxForDpi(UINT dpi);
int IconGapPxForDpi(UINT dpi);

}  // namespace traymirror
