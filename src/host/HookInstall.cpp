#include "HookInstall.h"

#include "Proto.h"

#include <tlhelp32.h>

#include <cstdio>
#include <filesystem>
#include <string>

// TrayHookProc is resolved with GetProcAddress, not a dllimport: the
// hook DLL is a MODULE library with no import library, so there is
// nothing to link against (ledger ruling, task 8). The signature matches
// the hook's extern "C" export.
using TrayHookProcFn = LRESULT(CALLBACK*)(int, WPARAM, LPARAM);

DWORD HookInstallFindTray(DWORD* explorerPid) {
  const HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
  if (tray == nullptr) {
    return 0;
  }
  DWORD pid = 0;
  const DWORD tid = GetWindowThreadProcessId(tray, &pid);
  if (explorerPid != nullptr) {
    *explorerPid = pid;
  }
  return tid;
}

bool HookInstallInstall(HookInstall* out, wchar_t* err, size_t errCap) {
  DWORD explorerPid = 0;
  const DWORD tid = HookInstallFindTray(&explorerPid);
  if (tid == 0) {
    _snwprintf_s(err, errCap, _TRUNCATE, L"Shell_TrayWnd not found");
    return false;
  }

  wchar_t exeDir[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, exeDir, MAX_PATH) == 0) {
    _snwprintf_s(err, errCap, _TRUNCATE, L"GetModuleFileNameW failed");
    return false;
  }
  auto dllPath =
      std::filesystem::path(exeDir).parent_path() / "TrayMirrorHook.dll";
  HMODULE dll = LoadLibraryW(dllPath.wstring().c_str());
  if (dll == nullptr) {
    _snwprintf_s(err, errCap, _TRUNCATE,
                 L"LoadLibraryW(TrayMirrorHook.dll) failed (%lu)",
                 GetLastError());
    return false;
  }

  const auto proc = reinterpret_cast<TrayHookProcFn>(
      GetProcAddress(dll, "TrayHookProc"));
  if (proc == nullptr) {
    _snwprintf_s(err, errCap, _TRUNCATE, L"TrayHookProc export missing (%lu)",
                 GetLastError());
    FreeLibrary(dll);
    return false;
  }

  HHOOK hook = SetWindowsHookExW(WH_CALLWNDPROCRET, proc, dll, tid);
  if (hook == nullptr) {
    _snwprintf_s(err, errCap, _TRUNCATE,
                 L"SetWindowsHookExW failed (%lu)", GetLastError());
    FreeLibrary(dll);
    return false;
  }

  out->dll = dll;
  out->hook = hook;
  out->explorerPid = explorerPid;
  return true;
}

void HookInstallUninstall(HookInstall* hi) {
  if (hi->hook != nullptr) {
    UnhookWindowsHookEx(hi->hook);
    hi->hook = nullptr;
  }
  if (hi->dll != nullptr) {
    FreeLibrary(hi->dll);
    hi->dll = nullptr;
  }
}

bool HookInstallResidentInExplorer(DWORD explorerPid) {
  if (explorerPid == 0) {
    return false;
  }
  const HANDLE snap =
      CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, explorerPid);
  if (snap == INVALID_HANDLE_VALUE) {
    return false;
  }
  MODULEENTRY32W me = {};
  me.dwSize = sizeof(me);
  bool resident = false;
  if (Module32FirstW(snap, &me) != 0) {
    do {
      if (_wcsicmp(me.szModule, L"TrayMirrorHook.dll") == 0) {
        resident = true;
        break;
      }
    } while (Module32NextW(snap, &me) != 0);
  }
  CloseHandle(snap);
  return resident;
}
