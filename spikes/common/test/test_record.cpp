// TDD test for the spike log record format (plan, task 2).
// Run: ctest -R TrayCommonTests --output-on-failure
#include "Record.h"
#include "TraySpike.h"

#include <cstdio>
#include <cstring>
#include <cwchar>

namespace {

int g_failures = 0;

void Check(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    ++g_failures;
  }
}

uint32_t RdU32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

uint64_t RdU64(const uint8_t* p) {
  uint64_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

size_t StrLenW(const wchar_t* s) {
  size_t n = 0;
  while (s[n] != L'\0') ++n;
  return n;
}

}  // namespace

int main() {
  // (a) The header layout is pinned: 32 bytes, pack(1).
  static_assert(sizeof(SpikeRecordHeader) == 32, "header size");

  // (b) Full layout: header + payload, every field at its offset.
  {
    uint8_t dst[64];
    const uint8_t payload[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    std::memset(dst, 0xEE, sizeof(dst));
    const size_t n = PackSpikeRecord(dst, sizeof(dst), /*qpc=*/1, /*dwData=*/1, payload, 4);
    Check(n == 36, "(b) returns 36");
    Check(RdU32(dst + 0) == kSpikeMagic, "(b) magic at 0");
    Check(RdU32(dst + 4) == 36, "(b) totalSize at 4");
    Check(RdU64(dst + 8) == 1, "(b) qpc at 8");
    Check(RdU64(dst + 16) == 1, "(b) dwData at 16");
    Check(RdU32(dst + 24) == 4, "(b) cbData at 24");
    Check(RdU32(dst + 28) == 0, "(b) flags at 28");
    Check(dst[32] == 0xAA && dst[33] == 0xBB && dst[34] == 0xCC && dst[35] == 0xDD,
          "(b) payload at 32");
  }

  // (c) Truncation: cbData > kSpikeMaxPayload → 4096 bytes + kFlagTruncated.
  {
    static const uint8_t big[kSpikeMaxPayload + 100] = {};
    uint8_t dst[32 + kSpikeMaxPayload];
    const size_t n = PackSpikeRecord(dst, sizeof(dst), 2, 1, big, kSpikeMaxPayload + 100);
    Check(n == 32 + kSpikeMaxPayload, "(c) truncated size");
    Check(RdU32(dst + 24) == kSpikeMaxPayload, "(c) cbData capped");
    Check(RdU32(dst + 28) == kFlagTruncated, "(c) truncated flag");
  }

  // (d) Insufficient dstCap → returns 0, dst untouched.
  {
    uint8_t dst[16];
    const uint8_t payload[4] = {1, 2, 3, 4};
    std::memset(dst, 0xEE, sizeof(dst));
    const size_t n = PackSpikeRecord(dst, sizeof(dst), 3, 1, payload, 4);
    Check(n == 0, "(d) returns 0 on small dstCap");
    bool untouched = true;
    for (uint8_t b : dst) untouched = untouched && (b == 0xEE);
    Check(untouched, "(d) dst untouched");
  }

  // (e) cbData=0 → a header-only record, still valid.
  {
    uint8_t dst[64];
    std::memset(dst, 0xEE, sizeof(dst));
    const size_t n = PackSpikeRecord(dst, sizeof(dst), 4, 1, nullptr, 0);
    Check(n == 32, "(e) empty payload size");
    Check(RdU32(dst + 4) == 32, "(e) totalSize 32");
    Check(RdU32(dst + 24) == 0, "(e) cbData 0");
    Check(RdU32(dst + 28) == 0, "(e) no flags");
  }

  // (f) GetSpikeLogPathW: %TEMP% + log file name (beyond the plan: covers the second function).
  {
    wchar_t path[MAX_PATH];
    Check(GetSpikeLogPathW(path, MAX_PATH) != FALSE, "(f) log path TRUE");
    const size_t len = StrLenW(path);
    const size_t nameLen = StrLenW(kSpikeLogFileName);
    Check(len > nameLen, "(f) path length");
    Check(len + nameLen <= MAX_PATH, "(f) fits buffer");
    Check(std::wcscmp(path + (len - nameLen), kSpikeLogFileName) == 0, "(f) ends with log name");
    wchar_t tiny[4];
    Check(GetSpikeLogPathW(tiny, 4) == FALSE, "(f) small buffer FALSE");
  }

  if (g_failures != 0) {
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
