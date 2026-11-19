#include "HostApp.h"

#include "HookInstall.h"
#include "Log.h"
#include "Proto.h"
#include "ReaderThread.h"
#include "Ring.h"
#include "RingReader.h"
#include "TrayPayload.h"
#include "ipc/SharedRing.h"
#include "ipc/SingleInstance.h"

#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

HWND DefaultFindTrayWnd() {
  return FindWindowW(L"Shell_TrayWnd", nullptr);
}

std::string Narrow(const wchar_t* s) {
  std::string out;
  for (; *s != L'\0'; ++s) {
    out.push_back(static_cast<char>(*s));
  }
  return out;
}

int ToInt(const wchar_t* s) {
  wchar_t* end = nullptr;
  const long v = wcstol(s, &end, 10);
  return end != nullptr && *end == L'\0' ? static_cast<int>(v) : -1;
}

// --- selftest -----------------------------------------------------------

struct SelftestApi {
  HMODULE dll = nullptr;
  LRESULT(CALLBACK* proc)(int, WPARAM, LPARAM) = nullptr;
};

void SelftestReport(const char* name, bool ok, int* failed) {
  std::printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
  if (!ok) {
    ++*failed;
  }
}

bool LoadHookApi(SelftestApi* api, wchar_t* err, size_t errCap) {
  wchar_t exe[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0) {
    _snwprintf_s(err, errCap, _TRUNCATE, L"GetModuleFileNameW failed");
    return false;
  }
  const std::filesystem::path dll =
      std::filesystem::path(exe).parent_path() / "TrayMirrorHook.dll";
  api->dll = LoadLibraryW(dll.wstring().c_str());
  if (api->dll == nullptr) {
    _snwprintf_s(err, errCap, _TRUNCATE, L"LoadLibraryW failed (%lu)",
                 GetLastError());
    return false;
  }
  api->proc = reinterpret_cast<decltype(api->proc)>(
      GetProcAddress(api->dll, "TrayHookProc"));
  if (api->proc == nullptr) {
    _snwprintf_s(err, errCap, _TRUNCATE, L"TrayHookProc export missing");
    FreeLibrary(api->dll);
    api->dll = nullptr;
    return false;
  }
  return true;
}

struct SelftestCall {
  COPYDATASTRUCT cds = {};
  CWPRETSTRUCT cwp = {};
};

void BuildSelftestCall(SelftestCall* c, DWORD dwData, const void* data,
                       DWORD cbData) {
  c->cds.dwData = dwData;
  c->cds.cbData = cbData;
  c->cds.lpData = const_cast<void*>(data);
  c->cwp.message = WM_COPYDATA;
  c->cwp.hwnd = reinterpret_cast<HWND>(1);
  c->cwp.lParam = reinterpret_cast<LPARAM>(&c->cds);
}

RingReadStatus SelftestRead(const SharedRingHandle& ring, RingRecordHeader* hdr,
                            const uint8_t** payload) {
  static uint8_t buf[kRecordHeaderSize + kMaxPayloadBytes +
                     kRecordFooterSize];
  return RingReaderGet(ring.header, ring.ring, buf, sizeof(buf), hdr,
                       payload);
}

}  // namespace

bool HostAppSelftest() {
  int failed = 0;
  wchar_t err[256] = {};

  // Kill switch residue guard (the latch from a previous arm must not
  // poison this run).
  HANDLE off = CreateEventW(nullptr, TRUE, FALSE, kOffEventName);
  if (off == nullptr) {
    std::printf("FAIL: CreateEventW(Off) (%lu)\n", GetLastError());
    return false;
  }
  ResetEvent(off);

  SharedRingHandle ring = {};
  if (!SharedRingCreate(&ring, GetCurrentProcessId(), err, 256)) {
    std::printf("FAIL: SharedRingCreate: %ls\n", err);
    CloseHandle(off);
    return false;
  }
  ring.header->writePos = 0;
  ring.header->readPos = 0;
  ring.header->hookAlive = 0;
  ring.header->droppedCount = 0;
  ring.header->flags = 0;

  SelftestApi api = {};
  if (!LoadHookApi(&api, err, 256)) {
    std::printf("FAIL: %ls\n", err);
    SharedRingClose(&ring);
    CloseHandle(off);
    return false;
  }

  // Scenario 1: a tray message lands in the ring, decodable.
  std::vector<uint8_t> tray(8 + kTrayPayloadSize, 0);
  {
    const uint32_t dw = 1;
    const uint32_t cb = kTrayPayloadSize;
    const uint32_t magic = kTrayMsgMagic;
    memcpy(tray.data(), &dw, 4);
    memcpy(tray.data() + 4, &cb, 4);
    memcpy(tray.data() + 8, &magic, 4);
    const wchar_t tip[] = L"selftest";
    memcpy(tray.data() + 8 + kTrayOffTip, tip, sizeof(tip));
  }
  {
    SelftestCall c;
    BuildSelftestCall(&c, 1, tray.data() + 8, kTrayPayloadSize);
    const uint32_t aliveBefore = ring.header->hookAlive;
    api.proc(0, 0, reinterpret_cast<LPARAM>(&c.cwp));
    RingRecordHeader hdr = {};
    const uint8_t* payload = nullptr;
    const RingReadStatus st = SelftestRead(ring, &hdr, &payload);
    bool ok = st == RingReadStatus::Ok && hdr.type == kRecNotifyCopydata;
    if (ok) {
      TrayMessage msg = {};
      ok = DecodeTrayPayload(payload, hdr.payloadSize, &msg) ==
               TrayDecodeStatus::Ok &&
           msg.nim == 0 && wcscmp(msg.tip, L"selftest") == 0;
    }
    SelftestReport("scenario 1: tray message recorded and decodable", ok,
                    &failed);
    SelftestReport("scenario 7: heartbeat advanced",
                   ring.header->hookAlive > aliveBefore, &failed);
  }

  // Scenario 2: cbData == 0 → mini-header-only record.
  {
    SelftestCall c;
    BuildSelftestCall(&c, 1, nullptr, 0);
    api.proc(0, 0, reinterpret_cast<LPARAM>(&c.cwp));
    RingRecordHeader hdr = {};
    const uint8_t* payload = nullptr;
    SelftestReport(
        "scenario 2: zero cbData records the mini header",
        SelftestRead(ring, &hdr, &payload) == RingReadStatus::Ok &&
            hdr.payloadSize == 8,
        &failed);
  }

  // Scenario 3: oversize cbData → truncated to the payload cap.
  {
    std::vector<uint8_t> big(kTrayPayloadSize, 0);
    SelftestCall c;
    BuildSelftestCall(&c, 1, big.data(), kMaxPayloadBytes * 4);
    api.proc(0, 0, reinterpret_cast<LPARAM>(&c.cwp));
    RingRecordHeader hdr = {};
    const uint8_t* payload = nullptr;
    SelftestReport(
        "scenario 3: oversize payload truncated",
        SelftestRead(ring, &hdr, &payload) == RingReadStatus::Ok &&
            hdr.payloadSize == kMaxPayloadBytes,
        &failed);
  }

  // Scenario 4: poisoned lpData → header-only record, process alive.
  {
    SYSTEM_INFO si = {};
    GetSystemInfo(&si);
    void* page = VirtualAlloc(nullptr, si.dwPageSize, MEM_COMMIT,
                              PAGE_NOACCESS);
    SelftestCall c;
    BuildSelftestCall(&c, 1, page, kTrayPayloadSize);
    api.proc(0, 0, reinterpret_cast<LPARAM>(&c.cwp));
    VirtualFree(page, 0, MEM_RELEASE);
    RingRecordHeader hdr = {};
    const uint8_t* payload = nullptr;
    SelftestReport(
        "scenario 4: poisoned lpData -> mini header, process alive",
        SelftestRead(ring, &hdr, &payload) == RingReadStatus::Ok &&
            DecodeTrayPayload(payload, hdr.payloadSize, nullptr) ==
                TrayDecodeStatus::Invalid,
        &failed);
  }

  // Scenario 5: dwData == 2 without diagnostic mode → filtered.
  {
    ring.header->flags = 0;
    SelftestCall c;
    BuildSelftestCall(&c, 2, tray.data() + 8, kTrayPayloadSize);
    const uint64_t pos = ring.header->writePos;
    api.proc(0, 0, reinterpret_cast<LPARAM>(&c.cwp));
    SelftestReport("scenario 5: non-tray subtype filtered without diag",
                   ring.header->writePos == pos, &failed);
  }

  // Scenario 6 (last: the latch is irreversible for this DLL instance):
  // armed kill switch → no-op, and the latch holds after ResetEvent.
  {
    SetEvent(off);
    SelftestCall c;
    BuildSelftestCall(&c, 1, tray.data() + 8, kTrayPayloadSize);
    const uint64_t pos = ring.header->writePos;
    api.proc(0, 0, reinterpret_cast<LPARAM>(&c.cwp));
    bool ok = ring.header->writePos == pos;
    ResetEvent(off);
    api.proc(0, 0, reinterpret_cast<LPARAM>(&c.cwp));
    ok = ok && ring.header->writePos == pos;
    SelftestReport("scenario 6: kill switch latches across reset", ok,
                   &failed);
  }

  FreeLibrary(api.dll);
  SharedRingClose(&ring);
  CloseHandle(off);

  if (failed != 0) {
    std::printf("SELFTEST FAILED (%d scenarios)\n", failed);
    return false;
  }
  std::printf("SELFTEST PASSED\n");
  return true;
}

// --- CLI -----------------------------------------------------------------

bool HostAppParseArgs(const wchar_t* const* argv, int argc,
                      HostAppConfig* out) {
  *out = HostAppConfig();
  for (int i = 0; i < argc; ++i) {
    const std::wstring a(argv[i]);
    if (a == L"--selftest") {
      out->selftest = true;
    } else if (a == L"--disable-hook") {
      out->disableHook = true;
    } else if (a == L"--diagnostic") {
      out->diagnostic = true;
    } else if (a == L"--duration" && i + 1 < argc) {
      out->durationSec = ToInt(argv[++i]);
      if (out->durationSec <= 0) {
        return false;
      }
    } else if (a == L"--log-level" && i + 1 < argc) {
      out->logLevel = std::string();
      const wchar_t* v = argv[i + 1];
      for (; *v != L'\0'; ++v) {
        out->logLevel.push_back(static_cast<char>(*v));
      }
      ++i;
    } else {
      return false;
    }
  }
  return true;
}

// --- the guarded run ------------------------------------------------------

namespace {

HANDLE g_stopEvent = nullptr;

BOOL WINAPI ConsoleCtrl(DWORD type) {
  (void)type;
  if (g_stopEvent != nullptr) {
    SetEvent(g_stopEvent);
  }
  return TRUE;
}

// The record log line, per plan task 8: the stage-0 measured fields,
// decoded — the stage 1 acceptance deliverable and the stage 2
// handoff read this format.
const char* NimName(uint32_t nim) {
  switch (nim) {
    case 0: return "ADD";
    case 1: return "MODIFY";
    case 2: return "DELETE";
    case 3: return "SETFOCUS";
    case 4: return "SETVERSION";
    default: return "UNKNOWN";
  }
}

}  // namespace

int HostAppRun(const HostAppConfig& config) {
  // (1) Single instance (spec 7.4).
  SingleInstance single;
  if (!single.Acquire()) {
    return 2;
  }

  if (config.disableHook) {
    // The manual kill switch (spec 7.3): create, arm, and KEEP the
    // handle for this process's lifetime. Closing it would destroy the
    // named event outright when no other handle exists, and the armed
    // state must stay observable while this process runs. The arm's
    // audience is a resident hook in explorer (orphaned by a host
    // crash); it holds its own handle, so the event outlives us there.
    HANDLE off = CreateEventW(nullptr, TRUE, FALSE, kOffEventName);
    if (off == nullptr) {
      return 4;
    }
    SetEvent(off);
    return 0;
  }

  // (2) The kill switch exists and is cleared BEFORE any hook
  // installation (spec 12.1; a latched switch from a previous session
  // must not silently kill this one).
  HANDLE off = CreateEventW(nullptr, TRUE, FALSE, kOffEventName);
  if (off == nullptr) {
    return 4;
  }
  ResetEvent(off);

  if (config.selftest) {
    CloseHandle(off);
    return HostAppSelftest() ? 0 : 5;
  }

  auto log = InitHostLogging(config.logLevel);

  const HWND tray =
      config.findTrayWnd != nullptr ? config.findTrayWnd() : DefaultFindTrayWnd();
  if (tray == nullptr) {
    log->error("Shell_TrayWnd not found — Explorer restarting or absent");
    CloseHandle(off);
    return 3;
  }

  // (3) The shared ring (adoption covers the crashed-host case).
  wchar_t err[256] = {};
  SharedRingHandle ring = {};
  if (!SharedRingCreate(&ring, GetCurrentProcessId(), err, 256)) {
    log->error("SharedRingCreate failed: {}", Narrow(err));
    CloseHandle(off);
    return 4;
  }
  if (config.diagnostic) {
    ring.header->flags |= kHostFlagDiag;
    log->info("diagnostic mode: non-tray subtypes are recorded too");
  }

  // (4) Hook installation, per-thread on the tray thread.
  HookInstall hi = {};
  if (!HookInstallInstall(&hi, err, 256)) {
    log->error("hook install failed: {}", Narrow(err));
    SharedRingClose(&ring);
    CloseHandle(off);
    return 4;
  }
  log->info("hook installed (explorer pid {})", hi.explorerPid);

  // (5) The reader thread.
  HANDLE readerStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  const HANDLE signal = ring.signalEvent;
  std::thread reader([header = ring.header, ringBytes = ring.ring, signal,
                      readerStop, &log]() {
    ReaderLoop(header, ringBytes, signal, readerStop,
               [&log](const RingRecordHeader&, const TrayMessage& msg) {
                 log->info(
                     "NIM_{} hwnd={:#x} uid={:#x} flags={:#x} "
                     "callback={:#x} icon={:#x} tip=\"{}\"",
                     NimName(msg.nim), msg.hwnd, msg.uid, msg.flags,
                     msg.callbackMsg, msg.icon, Narrow(msg.tip));
               });
  });

  // Event-driven idle (spec 12.9: no polling): the console handler sets
  // the stop event; --duration bounds the wait for scripted runs.
  HANDLE mainStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  g_stopEvent = mainStop;
  SetConsoleCtrlHandler(ConsoleCtrl, TRUE);
  if (config.durationSec != 0) {
    const ULONGLONG deadline =
        GetTickCount64() + (static_cast<ULONGLONG>(config.durationSec) * 1000ULL);
    for (;;) {
      const ULONGLONG now = GetTickCount64();
      if (now >= deadline) {
        break;
      }
      WaitForSingleObject(mainStop, static_cast<DWORD>(deadline - now));
      if (WaitForSingleObject(mainStop, 0) == WAIT_OBJECT_0) {
        break;
      }
    }
  } else {
    WaitForSingleObject(mainStop, INFINITE);
  }
  SetConsoleCtrlHandler(ConsoleCtrl, FALSE);
  g_stopEvent = nullptr;
  CloseHandle(mainStop);

  // (6) Clean shutdown: unhook first, then drain the reader, then close
  // the ring, then free the DLL.
  HookInstallUninstall(&hi);
  SetEvent(readerStop);
  SetEvent(signal);
  reader.join();
  CloseHandle(readerStop);

  // Self-verify the unload: the DLL must leave explorer.exe's module
  // list (the acceptance's own check).
  bool resident = true;
  for (int i = 0; i < 25 && resident; ++i) {  // up to ~5 s
    resident = HookInstallResidentInExplorer(hi.explorerPid);
    if (resident) {
      Sleep(200);
    }
  }
  log->info("hook unloaded from explorer: {}", resident ? "no" : "yes");

  SharedRingClose(&ring);
  CloseHandle(off);
  return 0;
}
