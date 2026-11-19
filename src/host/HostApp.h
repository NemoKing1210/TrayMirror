// Host application skeleton (plan task 8): CLI, the guarded start
// order (spec 12.1: the kill switch exists and is cleared BEFORE any
// hook installation), the reader thread lifecycle, clean shutdown with
// module-list verification, and the in-process hook selftest.
#pragma once

#include <windows.h>

#include <string>

struct HostAppConfig {
  bool selftest = false;
  bool disableHook = false;
  bool diagnostic = false;
  int durationSec = 0;  // 0: run until Ctrl+C / console close
  std::string logLevel;

  // Injectable for tests; the default finds the real Shell_TrayWnd.
  HWND (*findTrayWnd)() = nullptr;
};

// Parses argv[1..argc-1]. Returns false on unknown flags or missing
// values.
bool HostAppParseArgs(const wchar_t* const* argv, int argc,
                      HostAppConfig* out);

// Runs the host per the config; returns the process exit code:
// 0 ok, 2 another instance, 3 no tray window, 4 hook install failure,
// 5 selftest failed.
int HostAppRun(const HostAppConfig& config);

// The in-process hook gate (spec 12.1): exercises TrayMirrorHook.dll
// against a local ring — no Explorer contact. Returns true on pass.
bool HostAppSelftest();
