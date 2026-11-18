// TraySpikeCtl — the S1 spike controller (plan, task 4).
// selftest/run-safe: runs in its own process; start/stop/kill control the hook
// inside explorer.exe; broadcast-taskbarcreated — spike S3.
#include "Record.h"
#include "TraySpike.h"

#include <tlhelp32.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kHookDllName[] = L"TrayMirrorSpikeHook.dll";
constexpr wchar_t kTrayWndClass[] = L"Shell_TrayWnd";

// PROCESS_SYNCHRONIZE from winbase.h is not declared by this SDK set; the value
// is fixed in the OpenProcess/WaitForSingleObject documentation.
constexpr DWORD kProcessSynchronize = 0x00100000;

int PrintUsage() {
  std::printf(
      "usage: TraySpikeCtl <command>\n"
      "  selftest                synthetic TrayHookProc calls in own process (no Explorer)\n"
      "  start                   install hook on Shell_TrayWnd thread, daemon until stop\n"
      "  stop                    clean removal: Off event + UnhookWindowsHookEx\n"
      "  kill                    emergency: set Off event only (hook no-ops forever)\n"
      "  status                  daemon pid alive? hook dll in explorer modules?\n"
      "  broadcast-taskbarcreated  S3: SendNotifyMessage(HWND_BROADCAST, TaskbarCreated)\n");
  return 1;
}

std::wstring ExeDir() {
  wchar_t path[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return std::wstring();
  std::wstring dir(path, n);
  const size_t slash = dir.find_last_of(L"\\/");
  return (slash == std::wstring::npos) ? std::wstring() : dir.substr(0, slash);
}

std::wstring HookDllPath() { return ExeDir() + L"\\" + kHookDllName; }

std::wstring TempPathFor(const wchar_t* fileName) {
  wchar_t tmp[MAX_PATH];
  const DWORD n = GetTempPathW(MAX_PATH, tmp);
  if (n == 0 || n >= MAX_PATH) return std::wstring();
  return std::wstring(tmp, n) + fileName;
}

// --- log reading and parsing (task 2 format) --------------------------------

struct ParsedRecord {
  SpikeRecordHeader header;
  std::vector<uint8_t> payload;
};

std::vector<ParsedRecord> ReadLogRecords(const std::wstring& path) {
  std::vector<ParsedRecord> records;
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return records;

  std::vector<uint8_t> data;
  uint8_t chunk[8192];
  DWORD got = 0;
  for (;;) {
    if (!ReadFile(f, chunk, sizeof(chunk), &got, nullptr) || got == 0) break;
    data.insert(data.end(), chunk, chunk + got);
  }
  CloseHandle(f);

  size_t pos = 0;
  while (pos + sizeof(SpikeRecordHeader) <= data.size()) {
    SpikeRecordHeader h;
    memcpy(&h, &data[pos], sizeof(h));
    if (h.magic != kSpikeMagic || h.totalSize < sizeof(SpikeRecordHeader) ||
        h.totalSize > sizeof(SpikeRecordHeader) + kSpikeMaxPayload ||
        pos + h.totalSize > data.size()) {
      break;  // selftest: a valid log needs no resync (resync is task 7)
    }
    ParsedRecord r;
    r.header = h;
    r.payload.assign(data.begin() + pos + sizeof(h), data.begin() + pos + h.totalSize);
    records.push_back(std::move(r));
    pos += h.totalSize;
  }
  return records;
}

// --- selftest (plan, task 4: hook test WITHOUT contacting Explorer) ----------

int RunSelftest() {
  const std::wstring logPath = TempPathFor(kSpikeLogFileName);
  DeleteFileW(logPath.c_str());

  const std::wstring dll = HookDllPath();
  HMODULE hMod = LoadLibraryW(dll.c_str());
  if (hMod == nullptr) {
    std::printf("FAIL: LoadLibrary %ls: error %lu\n", dll.c_str(), GetLastError());
    return 5;
  }
  const auto proc = reinterpret_cast<HOOKPROC>(GetProcAddress(hMod, "TrayHookProc"));
  if (proc == nullptr) {
    std::printf("FAIL: no TrayHookProc export: error %lu\n", GetLastError());
    FreeLibrary(hMod);
    return 5;
  }

  uint8_t buf24[24];
  for (uint8_t i = 0; i < 24; ++i) buf24[i] = static_cast<uint8_t>(i + 1);
  static uint8_t big[8192];

  COPYDATASTRUCT cds;
  CWPRETSTRUCT cwp;
  cwp.hwnd = reinterpret_cast<HWND>(0x1234);
  cwp.message = WM_COPYDATA;
  cwp.wParam = 0;
  cwp.lParam = reinterpret_cast<LPARAM>(&cds);
  cwp.lResult = 0;

  // Scenarios 1–5 and 7 (order matters: scenario 6 latches the kill flag in the DLL).
  cds.dwData = 1; cds.cbData = 24;   cds.lpData = buf24; proc(0, 0, (LPARAM)&cwp);  // 1
  cds.dwData = 1; cds.cbData = 0;    cds.lpData = nullptr; proc(0, 0, (LPARAM)&cwp);  // 2
  cds.dwData = 1; cds.cbData = 8192; cds.lpData = big; proc(0, 0, (LPARAM)&cwp);  // 3
  cds.dwData = 1; cds.cbData = 24;   cds.lpData = nullptr; proc(0, 0, (LPARAM)&cwp);  // 4
  cds.dwData = 2; cds.cbData = 24;   cds.lpData = buf24; proc(0, 0, (LPARAM)&cwp);  // 5

  // Scenario 7 (review fix #3): lpData points to a PAGE_NOACCESS page.
  // Without the SEH guard, reading the payload crashes the process; the hook must
  // survive it and write a header-only record.
  void* poison = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
  if (poison == nullptr) {
    std::printf("FAIL: VirtualAlloc(PAGE_NOACCESS): error %lu\n", GetLastError());
    FreeLibrary(hMod);
    return 5;
  }
  cds.dwData = 1; cds.cbData = 24; cds.lpData = poison;
  proc(0, 0, (LPARAM)&cwp);  // 7
  VirtualFree(poison, 0, MEM_RELEASE);

  // Scenario 6: kill switch — no new records after the Off event is set.
  HANDLE hOff = CreateEventW(nullptr, TRUE, FALSE, kOffEventName);
  if (hOff == nullptr) {
    std::printf("FAIL: CreateEventW(Off): error %lu\n", GetLastError());
    FreeLibrary(hMod);
    return 5;
  }
  SetEvent(hOff);
  cds.dwData = 1; cds.cbData = 24; cds.lpData = buf24;
  proc(0, 0, (LPARAM)&cwp);

  FreeLibrary(hMod);  // DLL_PROCESS_DETACH closes the DLL's handles
  CloseHandle(hOff);

  // Log check: 6 records (scenario 6 writes nothing).
  const std::vector<ParsedRecord> recs = ReadLogRecords(logPath);
  int failures = 0;
  auto check = [&failures](bool cond, const char* what) {
    if (!cond) {
      std::printf("FAIL: scenario %s\n", what);
      ++failures;
    } else {
      std::printf("PASS: scenario %s\n", what);
    }
  };

  check(recs.size() == 6, "count: 6 records logged");
  if (recs.size() >= 6) {
    check(recs[0].header.dwData == 1 && recs[0].header.cbData == 24 &&
              recs[0].header.flags == 0 &&
              recs[0].payload.size() == 24 &&
              memcmp(recs[0].payload.data(), buf24, 24) == 0,
          "1: dwData=1, cbData=0x18, payload intact");
    check(recs[1].header.dwData == 1 && recs[1].header.cbData == 0 &&
              recs[1].header.flags == 0,
          "2: cbData=0, pData=NULL -> empty record");
    check(recs[2].header.dwData == 1 && recs[2].header.cbData == kSpikeMaxPayload &&
              recs[2].header.flags == kFlagTruncated,
          "3: cbData=8192 -> capped 4096 + truncated flag");
    check(recs[3].header.dwData == 1 && recs[3].header.cbData == 0 &&
              recs[3].header.flags == 0,
          "4: nonzero cbData with NULL pData -> cbData=0");
    check(recs[4].header.dwData == 2 && recs[4].header.cbData == 24,
          "5: dwData=2 logged as-is (non-tray)");
    check(recs[5].header.dwData == 1 && recs[5].header.cbData == 0 &&
              recs[5].header.flags == 0,
          "7: poisoned lpData -> header-only record, process alive");
  }

  if (failures != 0) {
    std::printf("SELFTEST FAILED: %d scenario check(s)\n", failures);
    return 5;
  }
  std::printf("SELFTEST PASSED\n");
  return 0;
}

// --- start / stop / kill ------------------------------------------------------

bool WritePidFile(DWORD pid) {
  const std::wstring path = TempPathFor(kPidFileName);
  wchar_t text[32];
  swprintf_s(text, L"%lu", static_cast<unsigned long>(pid));
  HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  const DWORD bytes = static_cast<DWORD>(wcslen(text) * sizeof(wchar_t));
  DWORD written = 0;
  const BOOL ok = WriteFile(f, text, bytes, &written, nullptr) && written == bytes;
  CloseHandle(f);
  return ok != FALSE;
}

void DeletePidFile() { DeleteFileW(TempPathFor(kPidFileName).c_str()); }

int RunStart() {
  HANDLE hMutex = CreateMutexW(nullptr, TRUE, kCtlMutexName);
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    std::printf("already running (mutex %ls)\n", kCtlMutexName);
    CloseHandle(hMutex);
    return 2;
  }

  // Review fix #1: create-or-open the Off event BEFORE installing the hook and KEEP
  // the handle for the whole session. Otherwise no event exists while the hook runs,
  // and the hook does a futile OpenEventW on every tray-thread message; a leftover
  // signaled state from a previous session is cleared with ResetEvent (plan, step 4c).
  HANDLE hOff = CreateEventW(nullptr, TRUE, FALSE, kOffEventName);
  if (hOff == nullptr) {
    std::printf("kill switch arm failed (CreateEventW Off): %lu\n", GetLastError());
    ReleaseMutex(hMutex);
    CloseHandle(hMutex);
    return 3;
  }
  ResetEvent(hOff);

  HWND tray = FindWindowW(kTrayWndClass, nullptr);
  if (tray == nullptr) {
    std::printf("Shell_TrayWnd not found\n");
    CloseHandle(hOff);
    ReleaseMutex(hMutex);
    CloseHandle(hMutex);
    return 3;
  }
  const DWORD tid = GetWindowThreadProcessId(tray, nullptr);

  const std::wstring dll = HookDllPath();
  HMODULE hMod = LoadLibraryW(dll.c_str());
  if (hMod == nullptr) {
    std::printf("LoadLibrary %ls failed: %lu\n", dll.c_str(), GetLastError());
    CloseHandle(hOff);
    ReleaseMutex(hMutex);
    CloseHandle(hMutex);
    return 4;
  }
  const auto proc = reinterpret_cast<HOOKPROC>(GetProcAddress(hMod, "TrayHookProc"));
  if (proc == nullptr) {
    std::printf("no TrayHookProc export: %lu\n", GetLastError());
    FreeLibrary(hMod);
    CloseHandle(hOff);
    ReleaseMutex(hMutex);
    CloseHandle(hMutex);
    return 4;
  }

  // Fresh log for every run.
  wchar_t logPath[MAX_PATH];
  if (GetSpikeLogPathW(logPath, MAX_PATH)) DeleteFileW(logPath);

  HHOOK hook = SetWindowsHookExW(WH_CALLWNDPROCRET, proc, hMod, tid);
  if (hook == nullptr) {
    std::printf("SetWindowsHookEx failed: %lu\n", GetLastError());
    FreeLibrary(hMod);
    CloseHandle(hOff);
    ReleaseMutex(hMutex);
    CloseHandle(hMutex);
    return 4;
  }

  if (!WritePidFile(GetCurrentProcessId())) {
    std::printf("warning: pid file not written\n");
  }
  std::printf("hook installed: hwnd=%p tid=%lu; waiting for stop event...\n", tray,
              static_cast<unsigned long>(tid));
  std::fflush(stdout);

  HANDLE hStop = CreateEventW(nullptr, FALSE, FALSE, kStopEventName);
  WaitForSingleObject(hStop, INFINITE);

  // The hOff handle has been held since the start of the session (review fix #1):
  // signal the event BEFORE unhooking — the hook is guaranteed to stop working.
  SetEvent(hOff);
  UnhookWindowsHookEx(hook);
  FreeLibrary(hMod);
  DeletePidFile();
  CloseHandle(hOff);
  CloseHandle(hStop);
  ReleaseMutex(hMutex);
  CloseHandle(hMutex);
  std::printf("hook removed\n");
  return 0;
}

int RunStop() {
  HANDLE hStop = OpenEventW(EVENT_MODIFY_STATE, FALSE, kStopEventName);
  if (hStop == nullptr) {
    std::printf("not running (no stop event)\n");
    return 3;
  }
  SetEvent(hStop);
  CloseHandle(hStop);
  std::printf("stop signaled; daemon removes hook\n");
  return 0;
}

int RunKill() {
  HANDLE hOff = CreateEventW(nullptr, TRUE, FALSE, kOffEventName);
  if (hOff == nullptr) {
    std::printf("CreateEventW(Off) failed: %lu\n", GetLastError());
    return 1;
  }
  SetEvent(hOff);
  CloseHandle(hOff);
  std::printf(
      "kill switch SET: hook no-ops forever (until DLL unload).\n"
      "run 'stop' if the daemon is alive, or restart Explorer to fully clean:\n"
      "  taskkill /f /im explorer.exe && start explorer.exe\n");
  return 0;
}

// --- status -------------------------------------------------------------------

bool IsDaemonAlive(DWORD* outPid) {
  const std::wstring path = TempPathFor(kPidFileName);
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  wchar_t text[32] = {};
  DWORD got = 0;
  ReadFile(f, text, sizeof(text) - sizeof(wchar_t), &got, nullptr);
  CloseHandle(f);
  if (got == 0) return false;
  const DWORD pid = static_cast<DWORD>(wcstoul(text, nullptr, 10));
  if (outPid != nullptr) *outPid = pid;

  HANDLE proc = OpenProcess(kProcessSynchronize, FALSE, pid);
  if (proc == nullptr) return false;
  const DWORD w = WaitForSingleObject(proc, 0);
  CloseHandle(proc);
  return w == WAIT_TIMEOUT;
}

bool IsHookDllInExplorer() {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return false;

  PROCESSENTRY32W pe;
  pe.dwSize = sizeof(pe);
  BOOL ok = Process32FirstW(snap, &pe);
  bool found = false;
  while (ok && !found) {
    if (_wcsicmp(pe.szExeFile, L"explorer.exe") == 0) {
      HANDLE mods = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                             pe.th32ProcessID);
      if (mods != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W me;
        me.dwSize = sizeof(me);
        if (Module32FirstW(mods, &me)) {
          do {
            if (_wcsicmp(me.szModule, kHookDllName) == 0) {
              found = true;
              break;
            }
          } while (Module32NextW(mods, &me));
        }
        CloseHandle(mods);
      }
    }
    ok = Process32NextW(snap, &pe);
  }
  CloseHandle(snap);
  return found;
}

int RunStatus() {
  DWORD pid = 0;
  if (IsDaemonAlive(&pid)) {
    std::printf("daemon: RUNNING (pid %lu)\n", static_cast<unsigned long>(pid));
  } else {
    std::printf("daemon: not running\n");
  }
  std::printf("hook dll in explorer: %s\n", IsHookDllInExplorer() ? "LOADED" : "absent");
  return 0;
}

// --- S3: TaskbarCreated broadcast ---------------------------------------------

int RunBroadcastTaskbarCreated() {
  const UINT msg = RegisterWindowMessageW(L"TaskbarCreated");
  if (msg == 0) {
    std::printf("RegisterWindowMessage failed: %lu\n", GetLastError());
    return 1;
  }
  SendNotifyMessageW(HWND_BROADCAST, msg, 0, 0);
  std::printf("TaskbarCreated (0x%04X) broadcast sent\n", msg);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) return PrintUsage();
  const std::string cmd(argv[1]);
  if (cmd == "selftest") return RunSelftest();
  if (cmd == "start") return RunStart();
  if (cmd == "stop") return RunStop();
  if (cmd == "kill") return RunKill();
  if (cmd == "status") return RunStatus();
  if (cmd == "broadcast-taskbarcreated") return RunBroadcastTaskbarCreated();
  return PrintUsage();
}
