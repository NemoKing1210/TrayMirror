// TrayMirrorSpikeHook — the minimal S1 spike hook (plan, task 3; ADR-0001).
//
// Spec 12.4 discipline: no STL, no new/malloc, no exceptions, no CRT file
// functions (kernel32 only), no blocking synchronization. memcpy is an MSVC intrinsic.
// Single responsibility: copy the WM_COPYDATA payload and write a record to the
// log file (see Record.h).
//
// The hook is installed per-thread on the Shell_TrayWnd thread (TraySpikeCtl,
// task 4), so the static buffers need no synchronization.
#include "Record.h"
#include "TraySpike.h"

#include <cstring>

namespace {

// Kill switch (spec 12.1): the Off event is checked on every call.
// If OpenEventW fails (no event exists), keep going without the kill switch:
// hook removal is then done by UnhookWindowsHookEx from TraySpikeCtl.
volatile LONG g_disabled = 0;
HANDLE g_hKillEvent = nullptr;
HANDLE g_hLogFile = nullptr;

uint8_t g_record[sizeof(SpikeRecordHeader) + kSpikeMaxPayload];
uint8_t g_payload[kSpikeMaxPayload];

}  // namespace

extern "C" __declspec(dllexport) LRESULT CALLBACK TrayHookProc(int code, WPARAM wParam,
                                                               LPARAM lParam) {
  if (code < 0) {
    return CallNextHookEx(nullptr, code, wParam, lParam);
  }

  // Lazy, once on SUCCESS: open the kill event. While ctl holds the handle
  // (the whole session), the event exists. If ctl died and the object was
  // destroyed, OpenEventW fails and is retried on every call: that is
  // deliberate, otherwise a later `ctl kill` (recreating the event) would
  // never affect the orphaned hook.
  if (g_hKillEvent == nullptr) {
    g_hKillEvent = OpenEventW(SYNCHRONIZE, FALSE, kOffEventName);
  }
  if (g_hKillEvent != nullptr && WaitForSingleObject(g_hKillEvent, 0) == WAIT_OBJECT_0) {
    // The event is manual-reset, but we latch it forever: after a kill the hook
    // never returns to work, even if someone resets the event.
    InterlockedExchange(&g_disabled, 1);
  }
  if (g_disabled != 0) {
    return CallNextHookEx(nullptr, code, wParam, lParam);
  }

  const CWPRETSTRUCT* cwp = reinterpret_cast<const CWPRETSTRUCT*>(lParam);
  if (cwp == nullptr || cwp->message != WM_COPYDATA) {
    return CallNextHookEx(nullptr, code, wParam, lParam);
  }
  const COPYDATASTRUCT* cds = reinterpret_cast<const COPYDATASTRUCT*>(cwp->lParam);
  if (cds == nullptr) {
    return CallNextHookEx(nullptr, code, wParam, lParam);
  }

  // Copy the payload into a static buffer. A NULL buffer from the sender is not
  // read: the record will have cbData=0. An oversized cbData is copied up to
  // kSpikeMaxPayload — PackSpikeRecord will set kFlagTruncated.
  //
  // Review fix #3: the lpData lifetime contract of WM_COPYDATA formally covers
  // only message processing, while WH_CALLWNDPROCRET runs AFTER the wndproc
  // returns. Raw SEH (not a C++ exception; spec, risk register: "SEH boundaries"):
  // an inaccessible buffer -> header-only record, the hook does not crash.
  const uint8_t* src = reinterpret_cast<const uint8_t*>(cds->lpData);
  uint32_t effCbData = cds->cbData;
  if (effCbData != 0 && src != nullptr) {
    const uint32_t want = (effCbData > kSpikeMaxPayload) ? kSpikeMaxPayload : effCbData;
    __try {
      memcpy(g_payload, src, want);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      effCbData = 0;  // lpData inaccessible: header-only record, same as NULL
    }
  } else {
    effCbData = 0;
  }

  uint64_t qpc = 0;
  QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&qpc));

  const size_t n =
      PackSpikeRecord(g_record, sizeof(g_record), qpc, cds->dwData, g_payload, effCbData);
  if (n == 0) {
    return CallNextHookEx(nullptr, code, wParam, lParam);
  }

  // Lazy, once: open the log in append mode. On failure the handle stays
  // INVALID_HANDLE_VALUE — no retries (one attempt per process, see ADR-0001).
  if (g_hLogFile == nullptr) {
    wchar_t path[MAX_PATH];
    if (GetSpikeLogPathW(path, MAX_PATH)) {
      g_hLogFile = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    } else {
      g_hLogFile = INVALID_HANDLE_VALUE;
    }
  }
  if (g_hLogFile != nullptr && g_hLogFile != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    WriteFile(g_hLogFile, g_record, static_cast<DWORD>(n), &written, nullptr);
  }

  return CallNextHookEx(nullptr, code, wParam, lParam);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
  switch (reason) {
    case DLL_PROCESS_ATTACH:
      DisableThreadLibraryCalls(hModule);
      break;
    case DLL_PROCESS_DETACH:
      if (g_hKillEvent != nullptr) CloseHandle(g_hKillEvent);
      if (g_hLogFile != nullptr && g_hLogFile != INVALID_HANDLE_VALUE) {
        CloseHandle(g_hLogFile);
      }
      break;
    default:
      break;
  }
  return TRUE;
}
