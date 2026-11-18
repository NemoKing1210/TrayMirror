#pragma once
// Spike log record format (plan, task 2; ADR-0001).
// C-compatible: used from HookDll — no STL, no CRT output.
#include <windows.h>

#include <cstddef>
#include <cstdint>

constexpr uint32_t kSpikeMagic = 0x524B5354;  // "TSKR" little-endian
constexpr uint32_t kSpikeMaxPayload = 4096;

enum SpikeRecordFlags : uint32_t { kFlagTruncated = 0x1 };

#pragma pack(push, 1)
struct SpikeRecordHeader {
  uint32_t magic;      // kSpikeMagic
  uint32_t totalSize;  // sizeof(SpikeRecordHeader) + bytes actually copied
  uint64_t qpc;        // QueryPerformanceCounter()
  uint64_t dwData;     // COPYDATASTRUCT.dwData
  uint32_t cbData;     // payload length in the record
  uint32_t flags;      // kFlagTruncated when truncated
};
#pragma pack(pop)
static_assert(sizeof(SpikeRecordHeader) == 32, "spike record header must be 32 bytes");

// Writes header + payload[0..min(cbData, kSpikeMaxPayload)) into dst.
// Returns the number of bytes written, or 0 if dstCap is insufficient (dst untouched).
size_t PackSpikeRecord(uint8_t* dst, size_t dstCap, uint64_t qpc, uint64_t dwData,
                       const uint8_t* payload, uint32_t cbData);

// Full log path: %TEMP% + kSpikeLogFileName. FALSE on a too-small buffer.
BOOL GetSpikeLogPathW(wchar_t* buf, DWORD cch);
