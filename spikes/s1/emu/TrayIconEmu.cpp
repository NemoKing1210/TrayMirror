// TrayIconEmu — tray icon emulator + message receiver (plan, task 5).
// Runs in its own process; contacts Explorer only through the public
// Shell_NotifyIconW (like any application) and does not install a hook.
#include <windows.h>
#include <shellapi.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr wchar_t kWndClass[] = L"TrayMirrorEmuWnd";
constexpr wchar_t kEmuLogFileName[] = L"TrayMirrorEmu.log";
constexpr wchar_t kDefaultTip[] = L"TRAYEMU";
// A fixed value that is easy to spot in dumps (S2).
constexpr UINT kCallbackMessage = WM_USER + 0x0FA0;
constexpr DWORD kModifyDelayMs = 500;

// --- command-line parameters ---------------------------------------------------

enum class Mode { kNone, kAdd, kModify, kDelete, kBalloon, kSetVersion, kRecv };

struct Options {
  Mode mode = Mode::kNone;
  std::wstring tip = kDefaultTip;
  int count = 1;
  UINT id = 1;
  DWORD durMs = 5000;
  bool v4 = false;
  bool hidden = false;
  bool guid = false;
  bool readdOnTaskbarCreated = false;
  std::wstring balloonTitle = L"EmuTitle";
  std::wstring balloonText = L"EmuText";
};

// Stage 2 payload-field measurement (plan task 1): a fixed, easily
// recognizable GUID so the dump can locate guidItem by its marker dwords.
constexpr GUID kGuidMarker = {0x11223344,
                              0x5566,
                              0x7788,
                              {0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00}};

// --- logging -------------------------------------------------------------------

HANDLE g_logFile = nullptr;

std::wstring EmuLogPath() {
  wchar_t tmp[MAX_PATH];
  const DWORD n = GetTempPathW(MAX_PATH, tmp);
  if (n == 0 || n >= MAX_PATH) return std::wstring();
  return std::wstring(tmp, n) + kEmuLogFileName;
}

void EmuLogf(const wchar_t* fmt, ...) {
  if (g_logFile == nullptr) {
    g_logFile = CreateFileW(EmuLogPath().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_logFile == INVALID_HANDLE_VALUE) {
      g_logFile = nullptr;
      return;
    }
  }
  wchar_t line[1024];
  uint64_t qpc = 0;
  QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&qpc));
  int len = swprintf_s(line, L"time=%llu ", qpc);
  va_list ap;
  va_start(ap, fmt);
  len += vswprintf_s(line + len, static_cast<size_t>(sizeof(line) / sizeof(wchar_t)) - len,
                     fmt, ap);
  va_end(ap);
  if (len < 0) return;
  line[len++] = L'\n';
  DWORD written = 0;
  WriteFile(g_logFile, line, static_cast<DWORD>(len * sizeof(wchar_t)), &written,
            nullptr);
}

const wchar_t* KnownMessageName(UINT msg, UINT callback, UINT taskbarCreated) {
  if (msg == taskbarCreated && taskbarCreated != 0) return L"TASKBARCREATED";
  if (msg == callback && callback != 0) return L"callback";
  if (msg == WM_CREATE) return L"WM_CREATE";
  if (msg == WM_DESTROY) return L"WM_DESTROY";
  if (msg == WM_NCDESTROY) return L"WM_NCDESTROY";
  if (msg == WM_TIMER) return L"WM_TIMER";
  return L"";
}

// --- emulator state -------------------------------------------------------------

struct Options g_opts;
UINT g_taskbarCreated = 0;  // RegisterWindowMessage(L"TaskbarCreated")
bool g_wow64 = false;
HWND g_hwnd = nullptr;

bool AddIcon(UINT uid, bool withBalloon) {
  NOTIFYICONDATAW nid = {};
  nid.cbSize = sizeof(nid);
  nid.hWnd = g_hwnd;
  nid.uID = uid;
  nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  nid.uCallbackMessage = kCallbackMessage;
  nid.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512) /*IDI_APPLICATION*/);
  wcscpy_s(nid.szTip, g_opts.tip.c_str());
  if (g_opts.hidden) {
    nid.uFlags |= NIF_STATE;
    nid.dwState = NIS_HIDDEN;
    nid.dwStateMask = NIS_HIDDEN;
  }
  if (g_opts.guid) {
    nid.uFlags |= NIF_GUID;
    nid.guidItem = kGuidMarker;
  }
  if (withBalloon) {
    nid.uFlags |= NIF_INFO;
    wcscpy_s(nid.szInfoTitle, g_opts.balloonTitle.c_str());
    wcscpy_s(nid.szInfo, g_opts.balloonText.c_str());
  }
  if (g_opts.guid) {
    EmuLogf(
        L"GUID MARKER flags=0x%X guid={11223344-5566-7788-99AA-BBCCDDEEFF00}",
        nid.uFlags);
  }
  const BOOL ok = Shell_NotifyIconW(NIM_ADD, &nid);
  EmuLogf(L"ICON ADD %s hwnd=0x%llX uid=%u tip=\"%s\" callback=0x%X v4=%d hidden=%d guid=%d wow64=%d",
          ok ? L"ok" : L"FAILED", reinterpret_cast<unsigned long long>(g_hwnd), uid,
          g_opts.tip.c_str(), kCallbackMessage, g_opts.v4 ? 1 : 0,
          g_opts.hidden ? 1 : 0, g_opts.guid ? 1 : 0, g_wow64 ? 1 : 0);
  std::printf("%s ADD uid=%u\n", ok ? "" : "FAILED:", uid);
  if (!ok) return false;
  if (g_opts.v4) {
    NOTIFYICONDATAW vn = {};
    vn.cbSize = sizeof(vn);
    vn.hWnd = g_hwnd;
    vn.uID = uid;
    vn.uVersion = NOTIFYICON_VERSION_4;
    const BOOL vok = Shell_NotifyIconW(NIM_SETVERSION, &vn);
    EmuLogf(L"SETVERSION v4 uid=%u %s", uid, vok ? L"ok" : L"FAILED");
  }
  return true;
}

bool ModifyIcon(UINT uid) {
  NOTIFYICONDATAW nid = {};
  nid.cbSize = sizeof(nid);
  nid.hWnd = g_hwnd;
  nid.uID = uid;
  nid.uFlags = NIF_TIP;
  wcscpy_s(nid.szTip, g_opts.tip.c_str());
  const BOOL ok = Shell_NotifyIconW(NIM_MODIFY, &nid);
  EmuLogf(L"ICON MODIFY uid=%u tip=\"%s\" %s", uid, g_opts.tip.c_str(),
          ok ? L"ok" : L"FAILED");
  std::printf("%s MODIFY uid=%u\n", ok ? "" : "FAILED:", uid);
  return ok != FALSE;
}

bool DeleteIcon(UINT uid) {
  NOTIFYICONDATAW nid = {};
  nid.cbSize = sizeof(nid);
  nid.hWnd = g_hwnd;
  nid.uID = uid;
  const BOOL ok = Shell_NotifyIconW(NIM_DELETE, &nid);
  EmuLogf(L"ICON DELETE uid=%u %s", uid, ok ? L"ok" : L"FAILED");
  std::printf("%s DELETE uid=%u\n", ok ? "" : "FAILED:", uid);
  return ok != FALSE;
}

void ReAddAllIcons() {
  int readded = 0;
  for (UINT uid = 1; uid <= static_cast<UINT>(g_opts.count); ++uid) {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = uid;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;  // NO NIF_INFO (S3 spec: no repeated balloons)
    nid.uCallbackMessage = kCallbackMessage;
    nid.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512) /*IDI_APPLICATION*/);
    wcscpy_s(nid.szTip, g_opts.tip.c_str());
    if (Shell_NotifyIconW(NIM_ADD, &nid)) ++readded;
  }
  EmuLogf(L"TASKBARCREATED re-add %d/%d (no NIF_INFO)", readded, g_opts.count);
}

// --- window ---------------------------------------------------------------------

LRESULT CALLBACK EmuWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  if (msg == g_taskbarCreated && g_opts.readdOnTaskbarCreated) {
    EmuLogf(L"msg=0x%04X name=TASKBARCREATED wParam=0x%llX lParam=0x%llX", msg,
            static_cast<unsigned long long>(wParam),
            static_cast<unsigned long long>(lParam));
    ReAddAllIcons();
    return 0;
  }
  const wchar_t* name = KnownMessageName(msg, kCallbackMessage, g_taskbarCreated);
  EmuLogf(L"msg=0x%04X name=%ls wParam=0x%llX lParam=0x%llX", msg, name,
          static_cast<unsigned long long>(wParam),
          static_cast<unsigned long long>(lParam));
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// --- message loop with waitable timers (no polling loops) -----------------------

void Pump(DWORD durMs, HANDLE hActionTimer) {
  HANDLE handles[2] = {nullptr, hActionTimer};
  const DWORD n = (hActionTimer != nullptr) ? 2 : 1;

  LARGE_INTEGER due;
  due.QuadPart = -static_cast<LONGLONG>(durMs) * 10000;  // 100 ns units, relative
  HANDLE hMain = CreateWaitableTimerW(nullptr, TRUE, nullptr);
  SetWaitableTimer(hMain, &due, 0, nullptr, nullptr, FALSE);
  handles[0] = hMain;

  bool done = false;
  bool acted = false;
  while (!done) {
    const DWORD w =
        MsgWaitForMultipleObjects(n, handles, FALSE, INFINITE, QS_ALLINPUT);
    if (w == WAIT_OBJECT_0) break;  // main timer (--dur expired)
    if (n == 2 && w == WAIT_OBJECT_0 + 1 && !acted) {
      acted = true;
      ModifyIcon(g_opts.id);  // modify scenario: ADD, then MODIFY after 500 ms
    }
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) {
        done = true;
        break;
      }
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  CancelWaitableTimer(hMain);
  CloseHandle(hMain);
  if (hActionTimer != nullptr) {
    CancelWaitableTimer(hActionTimer);
    CloseHandle(hActionTimer);
  }
}

// --- CLI --------------------------------------------------------------------------

void PrintUsage() {
  std::printf(
      "usage: TrayIconEmu <mode> [options]\n"
      "modes: add | modify | delete | balloon | setversion | recv\n"
      "options:\n"
      "  --tip <text>   tooltip (default TRAYEMU)\n"
      "  --count N      icons uid 1..N (add mode, default 1)\n"
      "  --id N         uid for modify/delete/balloon/setversion (default 1)\n"
      "  --dur ms       run duration, default 5000\n"
      "  --v4           NIM_SETVERSION NOTIFYICON_VERSION_4 after ADD\n"
      "  --hidden       NIF_STATE NIS_HIDDEN\n"
      "  --guid         NIF_GUID with the fixed marker GUID (payload measurement)\n"
      "  --readd-on-taskbarcreated   re-issue NIM_ADD (without NIF_INFO) on broadcast\n"
      "  --title T --text X  balloon mode\n");
}

bool ParseOptions(int argc, wchar_t** argv, Options* o) {
  if (argc < 2) return false;
  const std::wstring mode(argv[1]);
  if (mode == L"add") o->mode = Mode::kAdd;
  else if (mode == L"modify") o->mode = Mode::kModify;
  else if (mode == L"delete") o->mode = Mode::kDelete;
  else if (mode == L"balloon") o->mode = Mode::kBalloon;
  else if (mode == L"setversion") o->mode = Mode::kSetVersion;
  else if (mode == L"recv") o->mode = Mode::kRecv;
  else return false;

  for (int i = 2; i < argc; ++i) {
    const std::wstring a(argv[i]);
    if (a == L"--tip" && i + 1 < argc) o->tip = argv[++i];
    else if (a == L"--count" && i + 1 < argc) o->count = wcstol(argv[++i], nullptr, 10);
    else if (a == L"--id" && i + 1 < argc) o->id = static_cast<UINT>(wcstoul(argv[++i], nullptr, 10));
    else if (a == L"--dur" && i + 1 < argc) o->durMs = static_cast<DWORD>(wcstoul(argv[++i], nullptr, 10));
    else if (a == L"--v4") o->v4 = true;
    else if (a == L"--hidden") o->hidden = true;
    else if (a == L"--guid") o->guid = true;
    else if (a == L"--readd-on-taskbarcreated") o->readdOnTaskbarCreated = true;
    else if (a == L"--title" && i + 1 < argc) o->balloonTitle = argv[++i];
    else if (a == L"--text" && i + 1 < argc) o->balloonText = argv[++i];
    else return false;
  }
  // Review fix #4: setversion mode must set v4 regardless of the separate
  // --v4 flag (plan: "ADD then NIM_SETVERSION=4").
  if (o->mode == Mode::kSetVersion) o->v4 = true;
  return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  Options o;
  if (!ParseOptions(argc, argv, &o)) {
    PrintUsage();
    return 1;
  }
  g_opts = o;

  BOOL wow = FALSE;
  IsWow64Process(GetCurrentProcess(), &wow);
  g_wow64 = (wow != FALSE);
  g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

  WNDCLASSW wc = {};
  wc.lpfnWndProc = EmuWndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = kWndClass;
  if (RegisterClassW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    std::printf("RegisterClassW failed: %lu\n", GetLastError());
    return 1;
  }

  // A regular hidden top-level window (NOT message-only): it must receive the
  // TaskbarCreated broadcast (S3).
  g_hwnd = CreateWindowExW(0, kWndClass, L"TrayMirrorEmu", WS_OVERLAPPED, 0, 0, 0, 0,
                           nullptr, nullptr, wc.hInstance, nullptr);
  if (g_hwnd == nullptr) {
    std::printf("CreateWindow failed: %lu\n", GetLastError());
    return 1;
  }

  HANDLE hActionTimer = nullptr;
  switch (o.mode) {
    case Mode::kAdd:
      for (UINT uid = 1; uid <= static_cast<UINT>(o.count); ++uid) AddIcon(uid, false);
      break;
    case Mode::kModify:
      AddIcon(o.id, false);
      hActionTimer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
      {
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>(kModifyDelayMs) * 10000;
        SetWaitableTimer(hActionTimer, &due, 0, nullptr, nullptr, FALSE);
      }
      break;
    case Mode::kDelete:
      AddIcon(o.id, false);
      DeleteIcon(o.id);
      break;
    case Mode::kBalloon:
      AddIcon(o.id, true);
      break;
    case Mode::kSetVersion:
      AddIcon(o.id, false);
      break;
    case Mode::kRecv:
      break;
    default:
      break;
  }

  Pump(o.durMs, hActionTimer);

  // On exit, remove the live icons (already removed in delete mode).
  if (o.mode != Mode::kRecv && o.mode != Mode::kDelete) {
    if (o.mode == Mode::kAdd) {
      for (UINT uid = 1; uid <= static_cast<UINT>(o.count); ++uid) DeleteIcon(uid);
    } else {
      DeleteIcon(o.id);
    }
  }

  DestroyWindow(g_hwnd);
  if (g_logFile != nullptr && g_logFile != INVALID_HANDLE_VALUE) CloseHandle(g_logFile);
  return 0;
}
