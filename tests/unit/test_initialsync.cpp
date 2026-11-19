// Unit tests for initial synchronization (spec 4.3): the resync event,
// the manual signal, and the reader's onResync hook. No Explorer: the
// broadcast itself is never executed here — only the plumbing.
#include "core/InitialSync.h"

#include "HostApp.h"
#include "Ring.h"
#include "ReaderThread.h"
#include "ipc/SharedRing.h"

#include <windows.h>

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

namespace {

class InitialSyncTest : public ::testing::Test {
 protected:
  void SetUp() override {
    err_[0] = L'\0';
    ASSERT_TRUE(SharedRingCreate(&ring_, 999, err_, 256)) << err_;
    ring_.header->writePos = 0;
    ring_.header->readPos = 0;
    ring_.header->hookAlive = 0;
    ring_.header->droppedCount = 0;
    ring_.header->flags = 0;
  }
  void TearDown() override { SharedRingClose(&ring_); }

  SharedRingHandle ring_ = {};
  wchar_t err_[256] = {};
};

TEST_F(InitialSyncTest, SignalResyncSetsTheEvent) {
  // Create the event as a running host would, then signal it.
  HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, traymirror::kResyncEventName);
  ASSERT_NE(ev, nullptr);
  EXPECT_TRUE(traymirror::SignalResync());
  EXPECT_EQ(WaitForSingleObject(ev, 0), WAIT_OBJECT_0);
  CloseHandle(ev);

  // With no host holding the event, the signal reports false instead of
  // crashing.
  EXPECT_FALSE(traymirror::SignalResync());
}

TEST_F(InitialSyncTest, ReaderCallsOnResyncOnOverflow) {
  HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE signal = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  ASSERT_NE(stop, nullptr);
  ASSERT_NE(signal, nullptr);

  std::atomic<int> resyncs{0};
  std::thread reader([&]() {
    ReaderLoop(ring_.header, ring_.ring, signal, stop,
               [](const RingRecordHeader&, const TrayMessage&) {},
               nullptr, [&resyncs]() { ++resyncs; });
  });

  // A drop is recorded; the next wake-up must report it via onResync.
  ring_.header->droppedCount = 5;
  SetEvent(signal);
  for (int i = 0; i < 100 && resyncs.load() == 0; ++i) {
    Sleep(10);
  }
  const int seen = resyncs.load();

  SetEvent(stop);
  SetEvent(signal);  // wake the reader so it observes the stop
  reader.join();
  CloseHandle(stop);
  CloseHandle(signal);

  EXPECT_GE(seen, 1);
}

TEST_F(InitialSyncTest, ReaderCallsOnResyncOnSignal) {
  HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE signal = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE resync = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  ASSERT_NE(stop, nullptr);
  ASSERT_NE(signal, nullptr);
  ASSERT_NE(resync, nullptr);

  std::atomic<int> resyncs{0};
  std::thread reader([&]() {
    ReaderLoop(ring_.header, ring_.ring, signal, stop,
               [](const RingRecordHeader&, const TrayMessage&) {}, resync,
               [&resyncs]() { ++resyncs; });
  });

  SetEvent(resync);
  for (int i = 0; i < 100 && resyncs.load() == 0; ++i) {
    Sleep(10);
  }
  const int seen = resyncs.load();

  SetEvent(stop);
  SetEvent(signal);
  reader.join();
  CloseHandle(stop);
  CloseHandle(signal);
  CloseHandle(resync);

  EXPECT_GE(seen, 1);
}

TEST(HostAppArgs, ResyncFlagParsed) {
  HostAppConfig config;
  const wchar_t* argv[] = {L"--resync"};
  ASSERT_TRUE(HostAppParseArgs(argv, 1, &config));
  EXPECT_TRUE(config.resync);
}

}  // namespace
