// Unit tests for the host skeleton: CLI parsing, start order (single
// instance, kill switch, tray absence), and the reader loop (plan
// task 8). No live Explorer contact: the tray-window finder is
// injectable and every live-install path is stubbed out.
#include "HostApp.h"
#include "Proto.h"
#include "ReaderThread.h"
#include "Ring.h"
#include "RingWriter.h"
#include "ipc/SingleInstance.h"

#include <windows.h>

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

namespace {

HostAppConfig Parse(std::vector<const wchar_t*> args) {
  HostAppConfig cfg;
  EXPECT_TRUE(HostAppParseArgs(args.data(), static_cast<int>(args.size()),
                               &cfg));
  return cfg;
}

TEST(HostApp, ParsesFlags) {
  HostAppConfig cfg = Parse({L"--selftest"});
  EXPECT_TRUE(cfg.selftest);

  cfg = Parse({L"--disable-hook"});
  EXPECT_TRUE(cfg.disableHook);

  cfg = Parse({L"--diagnostic"});
  EXPECT_TRUE(cfg.diagnostic);

  cfg = Parse({L"--duration", L"5"});
  EXPECT_EQ(cfg.durationSec, 5);

  cfg = Parse({L"--log-level", L"debug"});
  EXPECT_EQ(cfg.logLevel, "debug");

  cfg = Parse({});
  EXPECT_FALSE(cfg.selftest);
  EXPECT_FALSE(cfg.disableHook);
  EXPECT_EQ(cfg.durationSec, 0);
}

TEST(HostApp, RejectsUnknownFlag) {
  HostAppConfig cfg;
  const wchar_t* args[] = {L"--frobnicate"};
  EXPECT_FALSE(HostAppParseArgs(args, 1, &cfg));
}

TEST(HostApp, RejectsDurationWithoutValue) {
  HostAppConfig cfg;
  const wchar_t* args[] = {L"--duration"};
  EXPECT_FALSE(HostAppParseArgs(args, 1, &cfg));
}

TEST(HostApp, MissingTrayWindowIsExit3) {
  HostAppConfig cfg;
  cfg.findTrayWnd = []() -> HWND { return nullptr; };
  EXPECT_EQ(HostAppRun(cfg), 3);
}

TEST(HostApp, SecondInstanceIsExit2) {
  SingleInstance first;
  ASSERT_TRUE(first.Acquire());

  HostAppConfig cfg;
  cfg.findTrayWnd = []() -> HWND { return nullptr; };  // never reached
  EXPECT_EQ(HostAppRun(cfg), 2);
}

TEST(HostApp, DisableHookArmsKillSwitch) {
  HostAppConfig cfg;
  cfg.disableHook = true;
  EXPECT_EQ(HostAppRun(cfg), 0);

  HANDLE off = OpenEventW(SYNCHRONIZE, FALSE, kOffEventName);
  ASSERT_NE(off, nullptr);
  EXPECT_EQ(WaitForSingleObject(off, 0), WAIT_OBJECT_0);
  // Do not leave the latched switch for other tests.
  ResetEvent(off);
  CloseHandle(off);
}

struct SinkRecord {
  uint32_t type = 0;
  uint32_t nim = 0;
};

TEST(ReaderLoop, DrainsRecordsAndSignalsSink) {
  RingHeader h = {};
  h.magic = kRingMagic;
  h.protoVersion = kProtoVersion;
  h.headerSize = sizeof(RingHeader);
  h.ringSize = 8192;  // three records of 24 + 1492 + 4 bytes each
  std::vector<uint8_t> ring(8192);

  // Three tray records via the wire convention: dwData=1, cbData=1484,
  // measured TRAYDATA inside.
  const uint32_t kTrayMagic = 0x34753423;
  for (uint32_t i = 0; i < 3; ++i) {
    std::vector<uint8_t> payload(8 + 1484, 0);
    uint32_t dw = 1;
    uint32_t cb = 1484;
    memcpy(payload.data(), &dw, 4);
    memcpy(payload.data() + 4, &cb, 4);
    memcpy(payload.data() + 8, &kTrayMagic, 4);
    uint32_t nim = i;
    memcpy(payload.data() + 12, &nim, 4);
    ASSERT_TRUE(RingWriterPut(&h, ring.data(), kRecNotifyCopydata, i, 0,
                              payload.data(), 1484 + 8));
  }

  HANDLE signal = CreateEventW(nullptr, FALSE, FALSE, kSignalEventName);
  HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  ASSERT_NE(signal, nullptr);
  ASSERT_NE(stop, nullptr);

  std::vector<SinkRecord> sink;
  std::atomic<size_t> got{0};
  HANDLE sinkReady = CreateEventW(nullptr, FALSE, FALSE, nullptr);

  std::thread reader([&] {
    ReaderLoop(
        &h, ring.data(), signal, stop,
        [&sink, &got, &sinkReady](const RingRecordHeader& hdr, uint32_t nim) {
          sink.push_back({hdr.type, nim});
          ++got;
          SetEvent(sinkReady);
        });
  });

  SetEvent(signal);
  // Up to 3 wakeups; auto-reset consumes one per drain.
  SetEvent(signal);
  SetEvent(signal);
  for (int spin = 0; got.load() < 3 && spin < 500; ++spin) {
    WaitForSingleObject(sinkReady, 10);
    SetEvent(signal);
  }
  ASSERT_EQ(got.load(), 3u);

  SetEvent(stop);
  SetEvent(signal);
  reader.join();

  ASSERT_EQ(sink.size(), 3u);
  EXPECT_EQ(sink[0].type, kRecNotifyCopydata);
  EXPECT_EQ(sink[2].nim, 2u);

  CloseHandle(signal);
  CloseHandle(stop);
  CloseHandle(sinkReady);
}

}  // namespace
