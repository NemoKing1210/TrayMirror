// Wire layout of the shared ring buffer (spec section 3.1). Packed byte
// layout shared between the hook (inside explorer.exe) and the host.
#pragma once

#include "Proto.h"

#include <cstdint>

#pragma pack(push, 1)

struct RingHeader {
  uint32_t magic;
  uint32_t protoVersion;
  uint32_t headerSize;
  uint32_t ringSize;
  volatile uint64_t writePos;         // owned by the hook (single writer)
  volatile uint64_t readPos;          // owned by the host reader
  volatile uint32_t hookAlive;        // heartbeat, incremented on events
  volatile uint32_t droppedCount;     // records dropped on overflow
  uint32_t explorerPid;
  uint32_t flags;                     // kHostFlagDiag
};
static_assert(sizeof(RingHeader) == 48, "spec 3.1 header layout");
static_assert(offsetof(RingHeader, writePos) == 16, "spec 3.1 header layout");

// Record types, as stored in RingRecordHeader.type. Plain constants, not
// an enum: the wire field is a fixed uint32_t by spec 3.1, and an enum
// would only invite narrowing tricks.
inline constexpr uint32_t kRecPad = 0;              // fills the ring tail so records never wrap mid-way
inline constexpr uint32_t kRecNotifyCopydata = 1;   // WM_COPYDATA with dwData == 1 (tray messages)
inline constexpr uint32_t kRecOtherCopydata = 2;    // other subtypes, diagnostic mode only
inline constexpr uint32_t kRecHookState = 3;        // hook attach/detach notifications

struct RingRecordHeader {
  uint32_t size;          // total record bytes: header + payload + CRC
  uint32_t type;
  uint64_t qpcTimestamp;
  uint32_t senderPid;
  uint32_t payloadSize;
};
static_assert(sizeof(RingRecordHeader) == kRecordHeaderSize,
              "record header layout");

#pragma pack(pop)

// A record on the wire: RingRecordHeader, then payload[payloadSize] bytes,
// then a uint32 CRC32 computed over the header bytes and the payload. The
// CRC field is the discriminator for resynchronization on torn records.
