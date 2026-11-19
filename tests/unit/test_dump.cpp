// Unit tests for the dump tool (plan task 9): a private ring snapshot
// must decode back to readable records, garbage between records is
// skipped by resync, a bad header is refused, and the CLI gates an
// absent mapping with exit 3.
#include "DumpCore.h"

#include "Proto.h"
#include "Ring.h"
#include "RingReader.h"
#include "RingWriter.h"
#include "TrayPayload.h"
#include "ipc/SharedRing.h"

#include <windows.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace {

// A whole mapping snapshot (header + ring bytes) owned by the test.
struct LocalRing {
  RingHeader h = {};
  std::vector<uint8_t> snap;

  explicit LocalRing(uint32_t ringSize)
      : snap(sizeof(RingHeader) + static_cast<size_t>(ringSize)) {
    h.magic = kRingMagic;
    h.protoVersion = kProtoVersion;
    h.headerSize = sizeof(RingHeader);
    h.ringSize = ringSize;
    h.explorerPid = 4242;
    h.hookAlive = 7;
    memcpy(snap.data(), &h, sizeof(h));
  }

  volatile RingHeader* header() {
    return reinterpret_cast<volatile RingHeader*>(snap.data());
  }
  volatile uint8_t* ring() {
    return snap.data() + sizeof(RingHeader);
  }

  // One tray record via the wire convention (dwData, cbData, TRAYDATA).
  bool Put(uint32_t nim, const wchar_t* tip) {
    std::vector<uint8_t> payload(8 + kTrayPayloadSize, 0);
    const uint32_t dw = 1;
    const uint32_t cb = kTrayPayloadSize;
    const uint32_t magic = kTrayMsgMagic;
    memcpy(payload.data(), &dw, 4);
    memcpy(payload.data() + 4, &cb, 4);
    memcpy(payload.data() + 8, &magic, 4);
    memcpy(payload.data() + 8 + kTrayOffNim, &nim, 4);
    memcpy(payload.data() + 8 + kTrayOffTip, tip,
           (wcslen(tip) + 1) * sizeof(wchar_t));
    return RingWriterPut(header(), ring(), kRecNotifyCopydata, 0, 0,
                         payload.data(), 8 + kTrayPayloadSize);
  }
};

std::string DumpToString(const LocalRing& lr, const DumpOptions& opt) {
  FILE* f = nullptr;
  if (tmpfile_s(&f) != 0 || f == nullptr) {
    return "<tmpfile failed>";
  }
  DumpSnapshot(lr.snap.data(), lr.snap.size(), opt, f);
  std::rewind(f);
  std::string out;
  char buf[256];
  size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) != 0) {
    out.append(buf, n);
  }
  std::fclose(f);
  return out;
}

TEST(DumpSnapshot, PrintsAllDecodedRecords) {
  LocalRing lr(8192);
  ASSERT_TRUE(lr.Put(0, L"alpha"));
  ASSERT_TRUE(lr.Put(1, L"beta"));
  ASSERT_TRUE(lr.Put(2, L"gamma"));

  DumpOptions opt;
  const std::string out = DumpToString(lr, opt);
  EXPECT_NE(out.find("tip=\"alpha\""), std::string::npos);
  EXPECT_NE(out.find("tip=\"beta\""), std::string::npos);
  EXPECT_NE(out.find("tip=\"gamma\""), std::string::npos);
  EXPECT_NE(out.find("records=3"), std::string::npos);
}

TEST(DumpSnapshot, LimitCapsTheRecordCount) {
  LocalRing lr(8192);
  ASSERT_TRUE(lr.Put(0, L"alpha"));
  ASSERT_TRUE(lr.Put(1, L"beta"));
  ASSERT_TRUE(lr.Put(2, L"gamma"));

  DumpOptions opt;
  opt.limit = 2;
  const std::string out = DumpToString(lr, opt);
  EXPECT_NE(out.find("tip=\"alpha\""), std::string::npos);
  EXPECT_NE(out.find("tip=\"beta\""), std::string::npos);
  EXPECT_EQ(out.find("tip=\"gamma\""), std::string::npos);
  EXPECT_NE(out.find("records=2"), std::string::npos);
}

TEST(DumpSnapshot, GarbageRecordIsSkippedByResync) {
  LocalRing lr(8192);
  ASSERT_TRUE(lr.Put(0, L"alpha"));
  ASSERT_TRUE(lr.Put(1, L"beta"));
  // Corrupt a byte inside the first record's payload: its CRC no longer
  // validates, so the reader must resync and still find "beta".
  lr.ring()[100] ^= 0xFF;

  DumpOptions opt;
  const std::string out = DumpToString(lr, opt);
  EXPECT_EQ(out.find("tip=\"alpha\""), std::string::npos);
  EXPECT_NE(out.find("tip=\"beta\""), std::string::npos);
  EXPECT_NE(out.find("records=1"), std::string::npos);
}

TEST(DumpSnapshot, EmptyRingIsClean) {
  LocalRing lr(8192);
  DumpOptions opt;
  const std::string out = DumpToString(lr, opt);
  EXPECT_NE(out.find("records=0"), std::string::npos);
}

TEST(DumpSnapshot, ForeignMagicIsRefused) {
  LocalRing lr(8192);
  lr.snap[0] ^= 0xFF;  // break the magic

  DumpOptions opt;
  const std::string out = DumpToString(lr, opt);
  EXPECT_NE(out.find("not a TrayMirror ring"), std::string::npos);
}

TEST(DumpSnapshot, SummaryReportsTheHeaderCounters) {
  LocalRing lr(8192);
  ASSERT_TRUE(lr.Put(0, L"alpha"));
  ASSERT_TRUE(lr.Put(1, L"beta"));

  DumpOptions opt;
  opt.summaryOnly = true;
  const std::string out = DumpToString(lr, opt);
  EXPECT_NE(out.find("records=2"), std::string::npos);
  EXPECT_NE(out.find("explorerPid=4242"), std::string::npos);
  EXPECT_NE(out.find("dropped=0"), std::string::npos);
}

// --- the CLI executable ---------------------------------------------------

int RunDump(const wchar_t* args, std::string* out) {
  wchar_t exe[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0) {
    return -1;
  }
  wchar_t* cut = wcsrchr(exe, L'\\');
  if (cut == nullptr) {
    return -1;
  }
  *cut = L'\0';  // ...\tests\unit
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

  std::wstring cmd =
      std::wstring(exe) + L"\\src\\tools\\TrayMirrorDump.exe " + args;

  SECURITY_ATTRIBUTES inherit = {};
  inherit.nLength = sizeof(inherit);
  inherit.bInheritHandle = TRUE;
  HANDLE readEnd = nullptr;
  HANDLE writeEnd = nullptr;
  if (CreatePipe(&readEnd, &writeEnd, &inherit, 0) == 0) {
    return -1;
  }
  SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = writeEnd;
  si.hStdError = writeEnd;
  PROCESS_INFORMATION pi = {};
  if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr,
                     nullptr, &si, &pi) == 0) {
    std::printf("CreateProcessW failed: %lu (cmd: %ls)\n", GetLastError(),
                cmd.c_str());
    CloseHandle(readEnd);
    CloseHandle(writeEnd);
    return -1;
  }
  CloseHandle(writeEnd);

  char buf[4096];
  DWORD n = 0;
  while (ReadFile(readEnd, buf, sizeof(buf), &n, nullptr) != 0 && n != 0) {
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

TEST(DumpCli, MissingMappingExitsThree) {
  // No ring is alive in this process at this point (fixtures close their
  // handles); a fresh boot has no mapping either.
  std::string out;
  const int code = RunDump(L"", &out);
  ASSERT_EQ(code, 3);
  EXPECT_NE(out.find("ring"), std::string::npos);
}

TEST(DumpCli, LiveMappingDumpsDecodedRecords) {
  wchar_t err[256] = {};
  SharedRingHandle ring = {};
  ASSERT_TRUE(SharedRingCreate(&ring, GetCurrentProcessId(), err, 256));
  ring.header->writePos = 0;
  ring.header->readPos = 0;
  ring.header->hookAlive = 0;
  ring.header->droppedCount = 0;
  ring.header->flags = 0;

  {
    std::vector<uint8_t> payload(8 + kTrayPayloadSize, 0);
    const uint32_t dw = 1;
    const uint32_t cb = kTrayPayloadSize;
    const uint32_t magic = kTrayMsgMagic;
    const uint32_t nim = 1;
    memcpy(payload.data(), &dw, 4);
    memcpy(payload.data() + 4, &cb, 4);
    memcpy(payload.data() + 8, &magic, 4);
    memcpy(payload.data() + 8 + kTrayOffNim, &nim, 4);
    const wchar_t tip[] = L"cli-visible";
    memcpy(payload.data() + 8 + kTrayOffTip, tip, sizeof(tip));
    ASSERT_TRUE(RingWriterPut(ring.header, ring.ring, kRecNotifyCopydata, 0,
                              0, payload.data(), 8 + kTrayPayloadSize));
  }

  std::string out;
  ASSERT_EQ(RunDump(L"", &out), 0);
  EXPECT_NE(out.find("tip=\"cli-visible\""), std::string::npos);
  EXPECT_NE(out.find("records=1"), std::string::npos);

  out.clear();
  ASSERT_EQ(RunDump(L"--mode summary", &out), 0);
  EXPECT_NE(out.find("records=1"), std::string::npos);
  EXPECT_NE(out.find("explorerPid="), std::string::npos);

  SharedRingClose(&ring);
}

}  // namespace
