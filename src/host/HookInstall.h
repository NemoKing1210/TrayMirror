// Hook installation from the host (spec 7.1): per-thread
// WH_CALLWNDPROCRET on the Shell_TrayWnd thread — never global.
#pragma once

#include <windows.h>

#include <cstddef>

struct HookInstall {
  HMODULE dll = nullptr;    // loaded TrayMirrorHook.dll
  HHOOK hook = nullptr;     // SetWindowsHookExW result
  DWORD explorerPid = 0;
};

// Finds the tray window; fills the Explorer PID. Returns the thread id,
// or 0 when there is no Shell_TrayWnd (Explorer restarting?).
DWORD HookInstallFindTray(DWORD* explorerPid);

// Loads the hook DLL (next to the host exe) and installs the hook on the
// Shell_TrayWnd thread. Returns false with a message in err on failure.
bool HookInstallInstall(HookInstall* out, wchar_t* err, size_t errCap);

// Unhooks and frees the DLL. Safe on a zeroed struct.
void HookInstallUninstall(HookInstall* hi);

// True while TrayMirrorHook.dll appears in explorer.exe's module list
// (public Toolhelp32Snapshot enumeration — the acceptance check's own
// method). Absence after uninstall means a clean unload.
bool HookInstallResidentInExplorer(DWORD explorerPid);
