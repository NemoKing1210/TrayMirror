#include "RecordCodec.h"

#include "Crc32.h"

#include <cstring>

size_t PackRingRecord(uint8_t* dst, size_t dstCap, uint32_t type, uint64_t qpc,
                      uint32_t senderPid, const uint8_t* payload,
                      uint32_t payloadSize) {
  if (payloadSize > kMaxPayloadBytes) {
    payloadSize = kMaxPayloadBytes;
  }
  const size_t total = kRecordHeaderSize + payloadSize + kRecordFooterSize;
  if (dstCap < total) {
    return 0;
  }

  RingRecordHeader hdr = {};
  hdr.size = static_cast<uint32_t>(total);
  hdr.type = type;
  hdr.qpcTimestamp = qpc;
  hdr.senderPid = senderPid;
  hdr.payloadSize = payloadSize;
  memcpy(dst, &hdr, sizeof(hdr));
  if (payloadSize != 0 && payload != nullptr) {
    memcpy(dst + kRecordHeaderSize, payload, payloadSize);
  }
  const uint32_t crc =
      Crc32(dst, kRecordHeaderSize + payloadSize);  // header + payload
  memcpy(dst + kRecordHeaderSize + payloadSize, &crc, sizeof(crc));
  return total;
}

bool ValidateRingRecord(const uint8_t* src, size_t srcCap,
                        RingRecordHeader* outHdr, const uint8_t** outPayload) {
  if (src == nullptr || srcCap < kRecordHeaderSize) {
    return false;
  }

  RingRecordHeader hdr = {};
  memcpy(&hdr, src, sizeof(hdr));

  // Size coherence first: everything below reads only within hdr.size,
  // which must fit the available bytes.
  if (hdr.size != kRecordHeaderSize + hdr.payloadSize + kRecordFooterSize) {
    return false;
  }
  if (hdr.size > srcCap) {
    return false;
  }
  if (hdr.payloadSize > kMaxPayloadBytes) {
    return false;
  }
  if (hdr.type > kRecHookState) {
    return false;
  }

  uint32_t stored = 0;
  memcpy(&stored, src + hdr.size - kRecordFooterSize, sizeof(stored));
  if (Crc32(src, hdr.size - kRecordFooterSize) != stored) {
    return false;
  }

  if (outHdr != nullptr) {
    *outHdr = hdr;
  }
  if (outPayload != nullptr) {
    *outPayload = src + kRecordHeaderSize;
  }
  return true;
}
