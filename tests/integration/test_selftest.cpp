// Integration test: the host executable's own gate — `TrayMirror.exe
// --selftest` exercises the hook DLL in THIS process tree and must exit
// 0 with SELFTEST PASSED (spec 12.1: the gate before any live install).
#include <windows.h>

#include <cstdio>
#include <string>

namespace {

int RunHost(const wchar_t* args, std::string* out) {
  wchar_t exe[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0) {
    return -1;
  }
  // exe = build\tests\integration\traymirror_selftest.exe
  wchar_t* cut = wcsrchr(exe, L'\\');
  if (cut == nullptr) {
    return -1;
  }
  *cut = L'\0';  // ...\tests\integration
  cut = wcsrchr(exe, L'\\');
  if (cut == nullptr) {
    return -1;
  }
  *cut = L'\0';  // ...\tests
  cut = wcsrchr(exe, L'\\');
  if (cut == nullptr) {
    return -1;
  }
  *cut = L'\0';  // ...\build

  std::wstring cmd = std::wstring(exe) + L"\\src\\host\\TrayMirror.exe " + args;

  SECURITY_ATTRIBUTES inherit = {};
  inherit.nLength = sizeof(inherit);
  inherit.bInheritHandle = TRUE;
  HANDLE readEnd = nullptr;
  HANDLE writeEnd = nullptr;
  CreatePipe(&readEnd, &writeEnd, &inherit, 0);
  SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = writeEnd;
  si.hStdError = writeEnd;
  PROCESS_INFORMATION pi = {};
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0,
                      nullptr, nullptr, &si, &pi)) {
    std::printf("CreateProcessW failed: %lu (cmd: %ls)\n", GetLastError(),
                cmd.c_str());
    CloseHandle(readEnd);
    CloseHandle(writeEnd);
    return -1;
  }
  CloseHandle(writeEnd);

  char buf[4096];
  DWORD n = 0;
  while (ReadFile(readEnd, buf, sizeof(buf), &n, nullptr) && n != 0) {
    out->append(buf, n);
  }
  CloseHandle(readEnd);
  WaitForSingleObject(pi.hProcess, 30000);
  DWORD code = 0;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return static_cast<int>(code);
}

}  // namespace

int wmain() {
  // Residue guard: a previous --disable-hook leaves the switch latched.
  HANDLE off = CreateEventW(nullptr, TRUE, FALSE, L"Local\\TrayMirror.Off");
  if (off != nullptr) {
    ResetEvent(off);
    CloseHandle(off);
  }

  std::string out;
  const int code = RunHost(L"--selftest", &out);
  if (code != 0) {
    std::printf("FAIL: selftest exit code %d\n--- output ---\n%s\n", code,
                out.c_str());
    return 1;
  }
  if (out.find("SELFTEST PASSED") == std::string::npos) {
    std::printf("FAIL: no SELFTEST PASSED marker\n--- output ---\n%s\n",
                out.c_str());
    return 1;
  }
  std::printf("PASS: TrayMirror.exe --selftest exited 0 with the marker\n");
  return 0;
}
