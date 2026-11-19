// Unit tests for the lock-free SPSC ring writer (hook side) and reader
// (host side) over a plain in-process buffer (plan task 4).
#include "Proto.h"
#include "RecordCodec.h"
#include "Ring.h"
#include "RingReader.h"
#include "RingWriter.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace {

constexpr uint32_t kTestRing = 512;

RingHeader MakeHeader(uint32_t ringSize) {
  RingHeader h = {};
  h.magic = kRingMagic;
  h.protoVersion = kProtoVersion;
  h.headerSize = sizeof(RingHeader);
  h.ringSize = ringSize;
  return h;
}

std::vector<uint8_t> Payload(uint32_t size, uint8_t seed) {
  std::vector<uint8_t> p(size);
  for (uint32_t i = 0; i < size; ++i) {
    p[i] = static_cast<uint8_t>(seed + static_cast<uint8_t>(i));
  }
  return p;
}

TEST(RingWriter, InitRejectsForeignHeader) {
  RingHeader h = MakeHeader(kTestRing);
  h.magic = 0xDEADBEEF;
  EXPECT_FALSE(RingWriterInit(&h));

  h = MakeHeader(kTestRing);
  h.protoVersion = kProtoVersion + 1;
  EXPECT_FALSE(RingWriterInit(&h));

  h = MakeHeader(kTestRing);
  EXPECT_TRUE(RingWriterInit(&h));
}

TEST(RingSpSc, PutGetRoundtrip) {
  RingHeader h = MakeHeader(kTestRing);
  std::vector<uint8_t> ring(kTestRing);
  const std::vector<uint8_t> payload = Payload(64, 0x11);

  ASSERT_TRUE(RingWriterPut(&h, ring.data(), kRecNotifyCopydata, 77, 42,
                           payload.data(), 64));
  EXPECT_EQ(h.hookAlive, 1u);
  EXPECT_GT(h.writePos, 0u);

  uint8_t buf[kRecordHeaderSize + kMaxPayloadBytes];
  RingRecordHeader hdr = {};
  const uint8_t* decoded = nullptr;
  EXPECT_EQ(RingReaderGet(&h, ring.data(), buf, sizeof(buf), &hdr, &decoded),
            RingReadStatus::Ok);
  EXPECT_EQ(hdr.type, kRecNotifyCopydata);
  EXPECT_EQ(hdr.qpcTimestamp, 77u);
  EXPECT_EQ(hdr.senderPid, 42u);
  EXPECT_EQ(hdr.payloadSize, 64u);
  EXPECT_EQ(memcmp(decoded, payload.data(), 64), 0);
  EXPECT_EQ(h.readPos, h.writePos);

  // Drained.
  EXPECT_EQ(RingReaderGet(&h, ring.data(), buf, sizeof(buf), &hdr, &decoded),
            RingReadStatus::Empty);
}

TEST(RingSpSc, OrderIsPreserved) {
  RingHeader h = MakeHeader(kTestRing);
  std::vector<uint8_t> ring(kTestRing);

  for (uint32_t i = 0; i < 8; ++i) {
    const std::vector<uint8_t> p = Payload(32, static_cast<uint8_t>(i));
    ASSERT_TRUE(RingWriterPut(&h, ring.data(), kRecNotifyCopydata, i, 0,
                             p.data(), 32));
  }

  uint8_t buf[kRecordHeaderSize + kMaxPayloadBytes];
  for (uint32_t i = 0; i < 8; ++i) {
    RingRecordHeader hdr = {};
    const uint8_t* decoded = nullptr;
    ASSERT_EQ(RingReaderGet(&h, ring.data(), buf, sizeof(buf), &hdr, &decoded),
              RingReadStatus::Ok);
    EXPECT_EQ(hdr.qpcTimestamp, i);
    EXPECT_EQ(decoded[0], static_cast<uint8_t>(i));
  }
}

TEST(RingSpSc, OverflowDropsAndCounts) {
  RingHeader h = MakeHeader(kTestRing);
  std::vector<uint8_t> ring(kTestRing);

  // Fill with records (92 bytes each) until the 512-byte ring rejects.
  const std::vector<uint8_t> p = Payload(64, 0xAB);
  size_t accepted = 0;
  while (RingWriterPut(&h, ring.data(), kRecNotifyCopydata, 1, 1, p.data(),
                       64)) {
    ++accepted;
  }
  EXPECT_GT(accepted, 0u);
  EXPECT_EQ(h.droppedCount, 1u);

  // One more attempt: counter grows, nothing written.
  EXPECT_FALSE(RingWriterPut(&h, ring.data(), kRecNotifyCopydata, 1, 1,
                             p.data(), 64));
  EXPECT_EQ(h.droppedCount, 2u);

  // Every accepted record survives in order.
  uint8_t buf[kRecordHeaderSize + kMaxPayloadBytes];
  for (size_t i = 0; i < accepted; ++i) {
    RingRecordHeader hdr = {};
    const uint8_t* decoded = nullptr;
    ASSERT_EQ(RingReaderGet(&h, ring.data(), buf, sizeof(buf), &hdr, &decoded),
              RingReadStatus::Ok);
    EXPECT_EQ(hdr.payloadSize, 64u);
  }
  RingRecordHeader hdr = {};
  const uint8_t* decoded = nullptr;
  EXPECT_EQ(RingReaderGet(&h, ring.data(), buf, sizeof(buf), &hdr, &decoded),
            RingReadStatus::Empty);
}

TEST(RingSpSc, WrapPadsWhenRecordDoesNotFitTail) {
  RingHeader h = MakeHeader(kTestRing);
  std::vector<uint8_t> ring(kTestRing);
  // Position the writer so the tail (512-480=32 bytes) is too short for
  // a 100-byte record but long enough for a pad record. The reader has
  // consumed everything before 480, so the whole ring is free.
  h.writePos = 480;
  h.readPos = 480;

  const std::vector<uint8_t> p = Payload(72, 0x22);
  ASSERT_TRUE(RingWriterPut(&h, ring.data(), kRecNotifyCopydata, 5, 0,
                           p.data(), 72));  // 72+24+4 = 100 bytes

  // The pad filled the 32-byte tail, the record wrapped to offset 0.
  EXPECT_EQ(h.writePos % kTestRing, 100u);

  uint8_t buf[kRecordHeaderSize + kMaxPayloadBytes];
  RingRecordHeader hdr = {};
  const uint8_t* decoded = nullptr;
  ASSERT_EQ(RingReaderGet(&h, ring.data(), buf, sizeof(buf), &hdr, &decoded),
            RingReadStatus::Ok);
  EXPECT_EQ(hdr.qpcTimestamp, 5u);
  EXPECT_EQ(memcmp(decoded, p.data(), 72), 0);
  EXPECT_EQ(h.readPos, h.writePos);
}

TEST(RingSpSc, TornRecordResyncsToNextValid) {
  RingHeader h = MakeHeader(kTestRing);
  std::vector<uint8_t> ring(kTestRing);
  // Simulate a writer crash: garbage occupies the first 40 bytes, then a
  // valid record sits behind it.
  memset(ring.data(), 0xE7, 40);
  h.writePos = 40;

  const std::vector<uint8_t> p = Payload(48, 0x33);
  ASSERT_TRUE(RingWriterPut(&h, ring.data(), kRecNotifyCopydata, 9, 0,
                           p.data(), 48));

  uint8_t buf[kRecordHeaderSize + kMaxPayloadBytes];
  RingRecordHeader hdr = {};
  const uint8_t* decoded = nullptr;
  EXPECT_EQ(RingReaderGet(&h, ring.data(), buf, sizeof(buf), &hdr, &decoded),
            RingReadStatus::Torn);

  std::vector<uint8_t> scratch(kTestRing);
  const uint64_t newPos = RingReaderResync(&h, ring.data(), scratch.data(),
                                           scratch.size());
  EXPECT_EQ(newPos, h.readPos);
  EXPECT_EQ(newPos, 40u);

  ASSERT_EQ(RingReaderGet(&h, ring.data(), buf, sizeof(buf), &hdr, &decoded),
            RingReadStatus::Ok);
  EXPECT_EQ(hdr.qpcTimestamp, 9u);
  EXPECT_EQ(memcmp(decoded, p.data(), 48), 0);
}

TEST(RingSpSc, ResyncSwallowsAllGarbage) {
  RingHeader h = MakeHeader(kTestRing);
  std::vector<uint8_t> ring(kTestRing, 0x55);
  h.writePos = 200;  // nothing valid anywhere

  std::vector<uint8_t> scratch(kTestRing);
  const uint64_t newPos = RingReaderResync(&h, ring.data(), scratch.data(),
                                           scratch.size());
  EXPECT_EQ(newPos, 200u);
  EXPECT_EQ(h.readPos, 200u);
}

TEST(RingSpSc, ReaderRejectsWrongVersion) {
  RingHeader h = MakeHeader(kTestRing);
  std::vector<uint8_t> ring(kTestRing);
  h.protoVersion = kProtoVersion + 3;
  h.writePos = 100;  // pretend content exists

  uint8_t buf[64];
  RingRecordHeader hdr = {};
  const uint8_t* decoded = nullptr;
  // Wrong version means the mapping is not ours: refuse, never guess.
  EXPECT_EQ(RingReaderGet(&h, ring.data(), buf, sizeof(buf), &hdr, &decoded),
            RingReadStatus::Invalid);
}

TEST(RingSpSc, MultithreadedSmoke) {
  RingHeader h = MakeHeader(kRingSizeBytes);
  std::vector<uint8_t> ring(kRingSizeBytes);

  std::atomic<bool> writerDone{false};
  std::atomic<size_t> received{0};
  std::atomic<uint64_t> lastQpc{0};

  std::thread reader([&] {
    uint8_t buf[kRecordHeaderSize + kMaxPayloadBytes];
    for (;;) {
      RingRecordHeader hdr = {};
      const uint8_t* decoded = nullptr;
      const RingReadStatus st =
          RingReaderGet(&h, ring.data(), buf, sizeof(buf), &hdr, &decoded);
      if (st == RingReadStatus::Ok) {
        ++received;
        uint64_t prev = lastQpc.load();
        while (hdr.qpcTimestamp > prev &&
               !lastQpc.compare_exchange_weak(prev, hdr.qpcTimestamp)) {
        }
      } else if (st == RingReadStatus::Empty && writerDone.load()) {
        break;
      }
    }
  });

  constexpr uint32_t kCount = 5000;
  for (uint32_t i = 0; i < kCount; ++i) {
    const std::vector<uint8_t> p = Payload(64, static_cast<uint8_t>(i));
    if (!RingWriterPut(&h, ring.data(), kRecNotifyCopydata, i, 0, p.data(),
                       64)) {
      // 1 MB ring, 5000 records of 92 bytes: must never overflow.
      FAIL() << "unexpected drop at " << i;
    }
  }
  writerDone = true;
  reader.join();

  EXPECT_EQ(received.load(), kCount);
  EXPECT_EQ(lastQpc.load(), kCount - 1);
}

}  // namespace
