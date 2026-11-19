// Integration test (own process): the kill switch latch. Once the hook
// has observed Local\TrayMirror.Off signaled, it stays inert for the
// rest of its load — even after the event is reset, a reloaded state is
// required (spec 12.1: the kill switch is the hard stop).
#include "Proto.h"
#include "Ring.h"
#include "RingReader.h"
#include "ipc/SharedRing.h"

#include <windows.h>

#include <cstdio>

namespace {

int Fail(const char* what) {
  std::printf("FAIL: %s (GetLastError=%lu)\n", what, GetLastError());
  return 1;
}

}  // namespace

int wmain() {
  SharedRingHandle ring = {};
  wchar_t err[256] = {};
  if (!SharedRingCreate(&ring, 1, err, 256)) {
    std::printf("FAIL: SharedRingCreate: %ls\n", err);
    return 1;
  }
  HANDLE off = CreateEventW(nullptr, TRUE, FALSE, kOffEventName);
  if (off == nullptr) {
    return Fail("CreateEventW(off)");
  }
  ResetEvent(off);

  HMODULE dll = LoadLibraryW(TRAYMIRROR_HOOK_DLL_PATH);
  if (dll == nullptr) {
    return Fail("LoadLibrary");
  }
  const auto proc = reinterpret_cast<LRESULT(CALLBACK*)(int, WPARAM, LPARAM)>(
      GetProcAddress(dll, "TrayHookProc"));
  if (proc == nullptr) {
    return Fail("TrayHookProc export");
  }

  COPYDATASTRUCT cds = {};
  cds.dwData = 1;
  cds.cbData = 0;  // mini-header-only record is enough to observe writes
  CWPRETSTRUCT cwp = {};
  cwp.message = WM_COPYDATA;
  cwp.hwnd = reinterpret_cast<HWND>(1);
  cwp.lParam = reinterpret_cast<LPARAM>(&cds);

  // Arm the kill switch, then deliver a message: nothing must be written.
  if (!SetEvent(off)) {
    return Fail("SetEvent(off)");
  }
  proc(0, 0, reinterpret_cast<LPARAM>(&cwp));
  if (ring.header->writePos != ring.header->readPos) {
    std::printf("FAIL: hook wrote while the kill switch was armed\n");
    return 1;
  }

  // Reset the event: the latch holds — still nothing.
  ResetEvent(off);
  proc(0, 0, reinterpret_cast<LPARAM>(&cwp));
  if (ring.header->writePos != ring.header->readPos) {
    std::printf("FAIL: the kill switch latch did not hold after ResetEvent\n");
    return 1;
  }

  std::printf("PASS: kill switch latch holds after ResetEvent\n");
  CloseHandle(off);
  SharedRingClose(&ring);
  FreeLibrary(dll);
  return 0;
}
