#include "RingReader.h"

#include "RecordCodec.h"

#include <cstring>
#include <intrin.h>

namespace {

constexpr uint32_t kMinRecordBytes =
    static_cast<uint32_t>(kRecordHeaderSize + kRecordFooterSize);

uint64_t VolatileLoad64(const volatile uint64_t* p) { return *p; }

void ReleaseStorePos(volatile uint64_t* pos, uint64_t value) {
  _InterlockedExchange64(reinterpret_cast<volatile __int64*>(pos),
                         static_cast<__int64>(value));
}

// Copies len bytes starting at ring offset (pos % ringSize), handling the
// wrap at the ring end. Returns false when the bytes do not fit scratch.
bool Linearize(volatile RingHeader* h, volatile uint8_t* ring, uint64_t pos,
               uint64_t len, uint8_t* scratch, size_t scratchCap) {
  if (len > scratchCap) {
    return false;
  }
  const uint32_t ringSize = h->ringSize;
  auto off = static_cast<size_t>(pos % ringSize);
  uint64_t done = 0;
  while (done < len) {
    const uint64_t chunk = len - done < (ringSize - off) ? len - done
                                                         : (ringSize - off);
    memcpy(scratch + done, const_cast<const uint8_t*>(ring + off),
           static_cast<size_t>(chunk));
    done += chunk;
    off = 0;
  }
  return true;
}

}  // namespace

RingReadStatus RingReaderGet(volatile RingHeader* h, volatile uint8_t* ring,
                             uint8_t* outBuf, uint32_t outCap,
                             RingRecordHeader* outHdr,
                             const uint8_t** outPayload) {
  if (h->magic != kRingMagic || h->protoVersion != kProtoVersion) {
    return RingReadStatus::Invalid;
  }

  for (;;) {
    const uint64_t writePos = VolatileLoad64(&h->writePos);
    const uint64_t readPos = VolatileLoad64(&h->readPos);
    if (readPos >= writePos) {
      return RingReadStatus::Empty;
    }

    // A record never starts in a tail shorter than header + CRC: those
    // bytes are dead pad space; skip to the ring start.
    const uint32_t ringSize = h->ringSize;
    const uint32_t tail =
        ringSize - static_cast<uint32_t>(readPos % ringSize);
    if (tail < kMinRecordBytes) {
      ReleaseStorePos(&h->readPos, readPos + tail);
      continue;
    }

    // Copy the candidate record out of shared memory, then validate the
    // private copy — never parse in place.
    const uint64_t avail = writePos - readPos;
    RingRecordHeader probe = {};
    memcpy(&probe, const_cast<const uint8_t*>(ring + (readPos % ringSize)),
           sizeof(probe));
    if (probe.size != kRecordHeaderSize + probe.payloadSize + kRecordFooterSize ||
        probe.size > avail || probe.size > outCap ||
        probe.payloadSize > kMaxPayloadBytes) {
      return RingReadStatus::Torn;
    }

    if (!Linearize(h, ring, readPos, probe.size, outBuf, outCap)) {
      return RingReadStatus::Torn;
    }
    RingRecordHeader hdr = {};
    const uint8_t* payload = nullptr;
    if (!ValidateRingRecord(outBuf, probe.size, &hdr, &payload)) {
      return RingReadStatus::Torn;
    }

    if (hdr.type == kRecPad) {
      // Pads are consumed, not delivered.
      ReleaseStorePos(&h->readPos, readPos + hdr.size);
      continue;
    }

    ReleaseStorePos(&h->readPos, readPos + hdr.size);
    if (outHdr != nullptr) {
      *outHdr = hdr;
    }
    if (outPayload != nullptr) {
      *outPayload = outBuf + kRecordHeaderSize;
    }
    return RingReadStatus::Ok;
  }
}

uint64_t RingReaderResync(volatile RingHeader* h, volatile uint8_t* ring,
                          uint8_t* scratch, size_t scratchCap) {
  const uint64_t writePos = VolatileLoad64(&h->writePos);
  uint64_t readPos = VolatileLoad64(&h->readPos);
  if (readPos >= writePos) {
    return readPos;
  }

  const uint64_t avail = writePos - readPos;
  if (!Linearize(h, ring, readPos, avail, scratch, scratchCap)) {
    // Region larger than the scratch: drop it all (spec 3.2, overflow
    // means resynchronization, not hangs).
    ReleaseStorePos(&h->readPos, writePos);
    return writePos;
  }

  RingRecordHeader hdr = {};
  const size_t found = FindNextRecord(scratch, static_cast<size_t>(avail), 0,
                                      &hdr);
  const uint64_t newPos =
      found == SIZE_MAX ? writePos : readPos + found;
  ReleaseStorePos(&h->readPos, newPos);
  return newPos;
}
