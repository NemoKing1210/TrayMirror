#include "Record.h"
#include "TraySpike.h"

#include <cstring>

// TDD GREEN step (plan, task 2).
// No CRT dependencies on the hot path (memcpy is an MSVC intrinsic),
// suitable for linking into HookDll (ADR-0001).

size_t PackSpikeRecord(uint8_t* dst, size_t dstCap, uint64_t qpc, uint64_t dwData,
                       const uint8_t* payload, uint32_t cbData) {
  const uint32_t stored = (cbData > kSpikeMaxPayload) ? kSpikeMaxPayload : cbData;
  const size_t needed = sizeof(SpikeRecordHeader) + static_cast<size_t>(stored);
  if (dst == nullptr || dstCap < needed) {
    return 0;
  }

  SpikeRecordHeader h = {};
  h.magic = kSpikeMagic;
  h.totalSize = static_cast<uint32_t>(needed);
  h.qpc = qpc;
  h.dwData = dwData;
  h.cbData = stored;
  h.flags = (stored != cbData) ? kFlagTruncated : 0u;

  memcpy(dst, &h, sizeof(h));
  if (stored != 0 && payload != nullptr) {
    memcpy(dst + sizeof(h), payload, stored);
  }
  return needed;
}

BOOL GetSpikeLogPathW(wchar_t* buf, DWORD cch) {
  if (buf == nullptr || cch == 0) {
    return FALSE;
  }
  const DWORD n = GetTempPathW(cch, buf);
  if (n == 0 || n >= cch) {
    return FALSE;
  }
  // GetTempPathW returns a path with a trailing backslash.
  size_t len = n;
  size_t nameLen = 0;
  while (kSpikeLogFileName[nameLen] != L'\0') ++nameLen;
  if (len + nameLen + 1 > cch) {
    return FALSE;
  }
  for (size_t i = 0; i <= nameLen; ++i) {
    buf[len + i] = kSpikeLogFileName[i];
  }
  return TRUE;
}
