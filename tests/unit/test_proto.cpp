// Unit tests for the stage 1 protocol core: CRC32, ring record pack and
// validation (plan task 3).
#include "Crc32.h"
#include "Proto.h"
#include "RecordCodec.h"
#include "Ring.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

TEST(Crc32, KnownVector) {
  const uint8_t kInput[] = "123456789";
  EXPECT_EQ(Crc32(kInput, 9), 0xCBF43926u);
}

TEST(Crc32, EmptyInput) {
  EXPECT_EQ(Crc32(nullptr, 0), 0u);  // CRC-32 of empty input (init^final)
}

std::vector<uint8_t> TestPayload(uint32_t size) {
  std::vector<uint8_t> p(size);
  for (uint32_t i = 0; i < size; ++i) p[i] = static_cast<uint8_t>(i * 7 + 1);
  return p;
}

TEST(RecordCodec, PackValidateRoundtrip) {
  const std::vector<uint8_t> payload = TestPayload(64);
  uint8_t buf[kRecordHeaderSize + kMaxPayloadBytes + 4] = {};

  const size_t packed = PackRingRecord(buf, sizeof(buf), kRecNotifyCopydata,
                                       0x1122334455667788ull, 4242,
                                       payload.data(), 64);
  ASSERT_GT(packed, kRecordHeaderSize + 4u);

  RingRecordHeader hdr = {};
  const uint8_t* decoded = nullptr;
  ASSERT_TRUE(ValidateRingRecord(buf, sizeof(buf), &hdr, &decoded));
  EXPECT_EQ(hdr.type, kRecNotifyCopydata);
  EXPECT_EQ(hdr.qpcTimestamp, 0x1122334455667788ull);
  EXPECT_EQ(hdr.senderPid, 4242u);
  EXPECT_EQ(hdr.payloadSize, 64u);
  EXPECT_EQ(hdr.size, packed);
  EXPECT_EQ(memcmp(decoded, payload.data(), 64), 0);
  EXPECT_EQ(packed, kRecordHeaderSize + 64u + 4u);
}

TEST(RecordCodec, PackTruncatesOversizePayload) {
  const std::vector<uint8_t> payload = TestPayload(kMaxPayloadBytes + 100);
  uint8_t buf[kRecordHeaderSize + kMaxPayloadBytes + 4] = {};

  const size_t packed = PackRingRecord(buf, sizeof(buf), kRecNotifyCopydata, 1,
                                       7, payload.data(),
                                       kMaxPayloadBytes + 100);
  EXPECT_EQ(packed, kRecordHeaderSize + kMaxPayloadBytes + 4u);

  RingRecordHeader hdr = {};
  const uint8_t* decoded = nullptr;
  ASSERT_TRUE(ValidateRingRecord(buf, sizeof(buf), &hdr, &decoded));
  EXPECT_EQ(hdr.payloadSize, kMaxPayloadBytes);
}

TEST(RecordCodec, PackNeedsExactCapacity) {
  const std::vector<uint8_t> payload = TestPayload(16);
  uint8_t buf[128] = {};

  const size_t packed = PackRingRecord(buf, sizeof(buf), kRecHookState, 1, 1,
                                       payload.data(), 16);
  ASSERT_GT(packed, 0u);
  // One byte short of the record: refuse without touching the buffer tail.
  EXPECT_EQ(PackRingRecord(buf, packed - 1, kRecHookState, 1, 1,
                           payload.data(), 16),
            0u);
}

TEST(RecordCodec, ValidateRejectsZeroBuffer) {
  uint8_t buf[kRecordHeaderSize + 16] = {};
  RingRecordHeader hdr = {};
  const uint8_t* decoded = nullptr;
  EXPECT_FALSE(ValidateRingRecord(buf, sizeof(buf), &hdr, &decoded));
}

TEST(RecordCodec, ValidateRejectsCrcMismatch) {
  const std::vector<uint8_t> payload = TestPayload(32);
  std::vector<uint8_t> buf(kRecordHeaderSize + 32 + 4);
  ASSERT_GT(PackRingRecord(buf.data(), buf.size(), kRecNotifyCopydata, 1, 1,
                           payload.data(), 32),
           0u);
  buf[kRecordHeaderSize + 5] ^= 0x80;  // corrupt one payload byte

  RingRecordHeader hdr = {};
  const uint8_t* decoded = nullptr;
  EXPECT_FALSE(ValidateRingRecord(buf.data(), buf.size(), &hdr, &decoded));
}

TEST(RecordCodec, ValidateRejectsBogusSizeFields) {
  // A header claiming a huge size must be rejected from the bytes that are
  // actually available, without reading past srcCap.
  std::vector<uint8_t> buf(64, 0xAB);
  const uint32_t kSizes[] = {0, 1, 0xFFFFFFFFu, 1000};
  for (const uint32_t size : kSizes) {
    memcpy(buf.data() + offsetof(RingRecordHeader, size), &size, sizeof(size));
    RingRecordHeader hdr = {};
    const uint8_t* decoded = nullptr;
    EXPECT_FALSE(ValidateRingRecord(buf.data(), buf.size(), &hdr, &decoded))
        << "size=" << size;
  }
}

TEST(RecordCodec, ValidateRejectsTruncatedBuffer) {
  const std::vector<uint8_t> payload = TestPayload(48);
  std::vector<uint8_t> buf(kRecordHeaderSize + 48 + 4);
  const size_t packed = PackRingRecord(buf.data(), buf.size(),
                                       kRecNotifyCopydata, 1, 1,
                                       payload.data(), 48);
  ASSERT_GT(packed, 0u);

  RingRecordHeader hdr = {};
  const uint8_t* decoded = nullptr;
  EXPECT_FALSE(ValidateRingRecord(buf.data(), packed - 1, &hdr, &decoded));
  // Sanity: the full buffer still validates.
  EXPECT_TRUE(ValidateRingRecord(buf.data(), packed, &hdr, &decoded));
}

TEST(RecordCodec, ValidateRejectsUnknownType) {
  const std::vector<uint8_t> payload = TestPayload(8);
  std::vector<uint8_t> buf(kRecordHeaderSize + 8 + 4);
  ASSERT_GT(PackRingRecord(buf.data(), buf.size(), kRecHookState, 1, 1,
                           payload.data(), 8),
           0u);
  uint32_t bogusType = 99;
  memcpy(buf.data() + offsetof(RingRecordHeader, type), &bogusType,
         sizeof(bogusType));

  RingRecordHeader hdr = {};
  const uint8_t* decoded = nullptr;
  EXPECT_FALSE(ValidateRingRecord(buf.data(), buf.size(), &hdr, &decoded));
}

TEST(RingHeader, WireLayoutMatchesSpec) {
  EXPECT_EQ(sizeof(RingHeader), 48u);  // spec 3.1 header sketch
}

}  // namespace
