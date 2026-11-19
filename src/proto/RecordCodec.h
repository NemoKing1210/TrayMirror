// Record pack/validate: pure functions over byte buffers (plan task 3).
// Pack builds the wire record; validate proves a byte range IS a record
// (size coherence, known type, payload cap, CRC) without ever reading
// past srcCap. Resync scanning over garbage lives on top of validate.
#pragma once

#include "Ring.h"

#include <cstddef>
#include <cstdint>

// Packs hdr(type, qpc, senderPid) + payload + trailing CRC32 into dst.
// A payload longer than kMaxPayloadBytes is truncated to that cap; the
// returned size reflects what was actually packed. Returns 0 when the
// record does not fit dstCap (nothing is written).
size_t PackRingRecord(uint8_t* dst, size_t dstCap, uint32_t type, uint64_t qpc,
                      uint32_t senderPid, const uint8_t* payload,
                      uint32_t payloadSize);

// Validates that src holds one complete, self-consistent record. On
// success fills outHdr and sets outPayload to the record's payload bytes
// (still inside src). Reads strictly within [src, src + srcCap).
bool ValidateRingRecord(const uint8_t* src, size_t srcCap,
                        RingRecordHeader* outHdr, const uint8_t** outPayload);
