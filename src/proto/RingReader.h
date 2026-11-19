// Lock-free SPSC ring reader — the host side. Always copies record bytes
// out of shared memory first and validates the private copy: a torn or
// hostile record can never be parsed in place (Review Focus 1).
#pragma once

#include "Ring.h"

#include <cstddef>
#include <cstdint>

enum class RingReadStatus : uint8_t {
  Empty,    // readPos == writePos
  Ok,       // one record copied into outBuf and validated
  Torn,     // structure at readPos does not validate (writer crash?) — resync
  Invalid,  // wrong magic/version: the mapping is not ours — refuse (spec 2.3.4)
};

// Reads one record (skipping pad records) into outBuf. On Ok, outHdr is
// filled and outPayload points inside outBuf. Advances readPos past the
// consumed record (release store).
RingReadStatus RingReaderGet(volatile RingHeader* h, volatile uint8_t* ring,
                             uint8_t* outBuf, uint32_t outCap,
                             RingRecordHeader* outHdr,
                             const uint8_t** outPayload);

// Resynchronization after Torn: linearizes the pending region into
// scratch, scans it for the next fully valid record (CRC as the
// discriminator), and release-stores readPos there — or at writePos when
// nothing valid remains. Returns the new readPos.
uint64_t RingReaderResync(volatile RingHeader* h, volatile uint8_t* ring,
                          uint8_t* scratch, size_t scratchCap);
