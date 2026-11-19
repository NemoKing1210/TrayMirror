// Lock-free SPSC ring writer — the hook side (single writer, on the
// Explorer tray thread). Kernel32-discipline code (spec 12.4): no STL, no
// allocations, no CRT calls; synchronization via Interlocked intrinsics
// only. Records never straddle the ring end: a too-short tail is filled
// with a pad record (or skipped outright when it cannot even hold a
// record header + CRC), and the writer wraps to offset 0.
#pragma once

#include "Ring.h"

#include <cstdint>

// Returns false when the header's magic or protocol version do not match
// this build: a foreign or stale mapping must never be written (spec 2.3.4
// — version mismatch means safe refusal, not best-effort).
bool RingWriterInit(volatile RingHeader* h);

// Appends one record; payload longer than kMaxPayloadBytes is truncated.
// Increments hookAlive (event-driven heartbeat, spec 3.2) on every call.
// Returns false and increments droppedCount when the record does not fit
// the free space (overflow policy, spec 3.2: drop new records).
bool RingWriterPut(volatile RingHeader* h, volatile uint8_t* ring,
                   uint32_t type, uint64_t qpc, uint32_t senderPid,
                   const uint8_t* payload, uint32_t payloadSize);
