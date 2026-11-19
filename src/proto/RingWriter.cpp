#include "RingWriter.h"

#include "Crc32.h"
#include "RecordCodec.h"

#include <intrin.h>

namespace {

// Minimum bytes a record (or pad) occupies: header + CRC, empty payload.
constexpr uint32_t kMinRecordBytes =
    static_cast<uint32_t>(kRecordHeaderSize + kRecordFooterSize);

uint64_t VolatileLoad64(const volatile uint64_t* p) {
  return *p;  // volatile read; the release stores below order it on x64
}

void WritePad(volatile RingHeader* h, volatile uint8_t* ring,
              uint32_t tail) {
  // A pad record fills the tail exactly so the next record starts at
  // offset 0. Payload of size tail-28 zero-filled.
  const uint32_t padPayload = tail - kMinRecordBytes;
  uint8_t pad[kMaxPayloadBytes];
  for (uint32_t i = 0; i < padPayload; ++i) {
    pad[i] = 0;
  }
  PackRingRecord(const_cast<uint8_t*>(ring + (h->writePos % h->ringSize)),
                 tail, kRecPad, 0, 0, padPayload != 0 ? pad : nullptr,
                 padPayload);
}

void ReleaseStorePos(volatile uint64_t* pos, uint64_t value) {
  // A locked op is a full fence on x64: the record bytes written before
  // this call are visible before the new position.
  _InterlockedExchange64(reinterpret_cast<volatile __int64*>(pos),
                        static_cast<__int64>(value));
}

}  // namespace

bool RingWriterInit(volatile RingHeader* h) {
  if (h == nullptr) {
    return false;
  }
  if (h->magic != kRingMagic || h->protoVersion != kProtoVersion) {
    return false;
  }
  // The ring must hold at least one record plus a pad; its actual size
  // (1 MB in production) is the creator's policy, not this check.
  return h->ringSize >= 2 * kMinRecordBytes;
}

bool RingWriterPut(volatile RingHeader* h, volatile uint8_t* ring,
                   uint32_t type, uint64_t qpc, uint32_t senderPid,
                   const uint8_t* payload, uint32_t payloadSize) {
  if (payloadSize > kMaxPayloadBytes) {
    payloadSize = kMaxPayloadBytes;
  }
  const uint32_t total =
      static_cast<uint32_t>(kRecordHeaderSize) + payloadSize +
      static_cast<uint32_t>(kRecordFooterSize);

  const uint64_t writePos = VolatileLoad64(&h->writePos);
  const uint64_t readPos = VolatileLoad64(&h->readPos);

  // Pad accounting: when the record does not fit the tail, the tail bytes
  // are consumed (pad record, or a bare skip when too short for any
  // record) before the record wraps to offset 0.
  const uint32_t tail =
      h->ringSize - static_cast<uint32_t>(writePos % h->ringSize);
  const uint64_t need = total + (tail < total ? tail : 0);

  if (writePos - readPos + need > h->ringSize) {
    _InterlockedIncrement(reinterpret_cast<volatile long*>(&h->droppedCount));
    _InterlockedIncrement(reinterpret_cast<volatile long*>(&h->hookAlive));
    return false;
  }

  if (tail < total) {
    if (tail >= kMinRecordBytes) {
      WritePad(h, ring, tail);
    }
    // Publish the pad/skip first, then the record lands at offset 0.
    ReleaseStorePos(&h->writePos, writePos + tail);
  }

  const uint64_t pos = VolatileLoad64(&h->writePos);
  PackRingRecord(const_cast<uint8_t*>(ring + (pos % h->ringSize)), total, type,
                 qpc, senderPid, payloadSize != 0 ? payload : nullptr,
                 payloadSize);
  // Release-publish: the record bytes are visible before the new
  // writePos (a locked op is a full fence on x64).
  ReleaseStorePos(&h->writePos, pos + total);
  _InterlockedIncrement(reinterpret_cast<volatile long*>(&h->hookAlive));
  return true;
}
