// TrayMirrorHook — the production hook DLL (spec sections 2.3, 3.2, 3.3,
// 12.4). Runs inside explorer.exe on the Shell_TrayWnd thread.
//
// Discipline (inviolable): no STL, no new/malloc, no C++ exceptions, no
// CRT file I/O, no blocking synchronization, kernel32 only. SEH
// boundaries around reads of caller-owned memory. All state is lazy and
// single-threaded (hook calls arrive on one Explorer thread), so plain
// statics are safe.
//
// Behavior: WM_COPYDATA with dwData == 1 is copied into the shared ring
// (kRecNotifyCopydata); other subtypes are recorded only in diagnostic
// mode (kHostFlagDiag, spec 3.3). The payload convention is the record
// mini-header: dwData + cbData, then up to kMaxPayloadBytes - 8 data
// bytes. The hook never interprets the data. Heartbeat: RingWriterPut
// increments hookAlive per call. The kill switch (Local\TrayMirror.Off)
// is checked on every call and latches forever once observed. A mapping
// whose header magic/version does not match this build is refused
// (spec 2.3.4). Missing ring/event handles are retried on later calls
// (an orphaned hook must not spin): open failures are cheap.
//
// Measured on Win11 build 26300 (stage 1 acceptance diagnostics,
// 2026-11-19): WH_CALLWNDPROCRET delivers tray WM_COPYDATA with a
// NONZERO wParam; filtering on the documented "wParam == 0 for messages
// from another process" reading drops 100% of real tray traffic. The
// hook does NOT filter on wParam — the S1 spike never did and captured
// both the emulator and real applications
// (docs/spikes/01-s1-hook-visibility.md).
#include <windows.h>

#include "Proto.h"
#include "Ring.h"
#include "RingWriter.h"

#include <cstring>

extern "C" __declspec(dllexport) LRESULT CALLBACK TrayHookProc(int code,
                                                               WPARAM wp,
                                                               LPARAM lp);

namespace {

struct HookState {
  HANDLE map = nullptr;        // Local\TrayMirror.Ring.v1 (FILE_MAP_WRITE)
  HANDLE signal = nullptr;     // Local\TrayMirror.Signal (SetEvent)
  HANDLE off = nullptr;        // Local\TrayMirror.Off (kill switch)
  volatile RingHeader* header = nullptr;
  volatile uint8_t* ring = nullptr;
  volatile LONG killed = 0;    // kill switch latch
};

HookState g_state = {};

// Payload mini-header + data. dwData/cbData first, then the copy data.
uint8_t g_payload[kMaxPayloadBytes] = {};

// Tries once per call to (re)acquire the ring mapping and the events.
// All failures are silent: an orphaned hook keeps the cost at one cheap
// kernel32 call per message.
void AcquireObjects(HookState* st) {
  if (st->map == nullptr) {
    HANDLE map = OpenFileMappingW(FILE_MAP_WRITE, FALSE, kRingName);
    if (map == nullptr) {
      return;
    }
    auto* view = static_cast<volatile uint8_t*>(
        MapViewOfFile(map, FILE_MAP_WRITE, 0, 0, 0));
    if (view == nullptr) {
      CloseHandle(map);
      return;
    }
    st->map = map;
    st->header = reinterpret_cast<volatile RingHeader*>(view);
    st->ring = view + sizeof(RingHeader);
  }
  if (st->signal == nullptr) {
    st->signal = OpenEventW(EVENT_MODIFY_STATE, FALSE, kSignalEventName);
  }
  if (st->off == nullptr) {
    st->off = OpenEventW(SYNCHRONIZE, FALSE, kOffEventName);
  }
}

// Copies the mini-header, then up to cap data bytes under an SEH
// boundary: a poisoned lpData (caller freed/guarded it) yields a
// header-only record instead of a crash. The hook MUST NOT interpret.
uint32_t CopyPayload(const COPYDATASTRUCT* cds, uint8_t* dst,
                     uint32_t cap) {
  uint32_t eff = 0;
  __try {
    const uint32_t want = cds->cbData < cap ? cds->cbData : cap;
    if (want != 0 && cds->lpData != nullptr) {
      memcpy(dst, cds->lpData, want);
    }
    eff = want;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    eff = 0;
  }
  return eff;
}

void HandleCopydata(const COPYDATASTRUCT* cds, HookState* st) {
  const bool tray = cds->dwData == 1;
  const bool diag = (st->header->flags & kHostFlagDiag) != 0;
  if (!tray && !diag) {
    return;  // spec 3.3: ignore other subtypes outside diagnostic mode
  }

  // dwData + cbData first, then the data (mini-header convention).
  uint32_t* mini = reinterpret_cast<uint32_t*>(g_payload);
  mini[0] = static_cast<uint32_t>(cds->dwData);
  mini[1] = static_cast<uint32_t>(cds->cbData);
  const uint32_t cap = kMaxPayloadBytes - 8;
  const uint32_t copied = CopyPayload(cds, g_payload + 8, cap);

  const uint64_t qpc = []() {
    LARGE_INTEGER t = {};
    QueryPerformanceCounter(&t);
    return static_cast<uint64_t>(t.QuadPart);
  }();

  if (RingWriterPut(st->header, st->ring, tray ? kRecNotifyCopydata
                                                : kRecOtherCopydata,
                    qpc, 0, g_payload, 8 + copied) &&
      st->signal != nullptr) {
    SetEvent(st->signal);
  }
}

}  // namespace

// DLL entry. On PROCESS_ATTACH the kill switch event is created/open
// and held for the DLL's lifetime: a `--disable-hook` arm must stay
// observable from the moment the hook exists anywhere, including the
// window before the first tray message (the lazy per-message open
// would let the named event object die with the arming process before
// an orphaned hook ever sees it). On PROCESS_DETACH the IPC handles
// are released: inside explorer.exe that fires only after the host has
// unhooked and the loader dropped the last reference, and in the
// unit/selftest process it keeps fixtures isolated. CreateEventW and
// the release calls are non-blocking kernel32 calls: allowed under
// the loader lock (spec 12.4).
BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_state.off = CreateEventW(nullptr, TRUE, FALSE, kOffEventName);
  }
  if (reason == DLL_PROCESS_DETACH) {
    if (g_state.header != nullptr) {
      UnmapViewOfFile(
          const_cast<uint8_t*>(reinterpret_cast<const volatile uint8_t*>(
              g_state.header)));
      g_state.header = nullptr;
      g_state.ring = nullptr;
    }
    if (g_state.map != nullptr) {
      CloseHandle(g_state.map);
      g_state.map = nullptr;
    }
    if (g_state.signal != nullptr) {
      CloseHandle(g_state.signal);
      g_state.signal = nullptr;
    }
    if (g_state.off != nullptr) {
      CloseHandle(g_state.off);
      g_state.off = nullptr;
    }
  }
  return TRUE;
}

LRESULT CALLBACK TrayHookProc(int code, WPARAM wp, LPARAM lp) {
  (void)wp;  // measured nonzero for tray traffic; must not filter (see
             // the file header note)
  if (code == HC_ACTION) {
    HookState* st = &g_state;

    // Kill switch first (spec 12.1): once observed, inert forever.
    if (st->killed == 0) {
      AcquireObjects(st);
      if (st->off != nullptr &&
          WaitForSingleObject(st->off, 0) == WAIT_OBJECT_0) {
        InterlockedExchange(&st->killed, 1);
      }
    }
    if (st->killed != 0) {
      return CallNextHookEx(nullptr, code, wp, lp);
    }

    if (st->map == nullptr || st->header == nullptr) {
      // No ring yet (Host not running) — nothing to do.
      return CallNextHookEx(nullptr, code, wp, lp);
    }

    // Version discipline (spec 2.3.4): refuse a foreign mapping.
    if (!RingWriterInit(st->header)) {
      return CallNextHookEx(nullptr, code, wp, lp);
    }

    if (lp != 0) {
      const auto* cwp = reinterpret_cast<const CWPRETSTRUCT*>(lp);
      if (cwp->message == WM_COPYDATA && cwp->lParam != 0) {
        HandleCopydata(reinterpret_cast<const COPYDATASTRUCT*>(cwp->lParam),
                       st);
      }
    }
  }
  return CallNextHookEx(nullptr, code, wp, lp);
}
