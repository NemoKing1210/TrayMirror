// TraySpikeDump — spike log record dump/statistics (plan, task 7).
#include "Layout.h"
#include "Parse.h"
#include "Record.h"
#include "TraySpike.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

uint32_t RdU32(const uint8_t* p) {
  uint32_t v;
  memcpy(&v, p, sizeof(v));
  return v;
}

uint64_t RdU64(const uint8_t* p) {
  uint64_t v;
  memcpy(&v, p, sizeof(v));
  return v;
}

std::vector<uint8_t> ReadAllBytes(const std::wstring& path) {
  std::vector<uint8_t> data;
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return data;
  uint8_t chunk[8192];
  DWORD got = 0;
  for (;;) {
    if (!ReadFile(f, chunk, sizeof(chunk), &got, nullptr) || got == 0) break;
    data.insert(data.end(), chunk, chunk + got);
  }
  CloseHandle(f);
  return data;
}

// --- words: offset / bytes / u32 / u64 / ascii grid ----------------------------

struct WordsCtx {
  size_t index = 0;
  size_t limit = 0;  // 0 = no limit
};

bool WordsSink(const SpikeRecordHeader* hdr, const uint8_t* payload, void* user) {
  auto* ctx = static_cast<WordsCtx*>(user);
  if (hdr == nullptr) {
    std::printf("  [sync lost: torn record]\n");
    return true;
  }
  if (ctx->limit != 0 && ctx->index >= ctx->limit) return false;
  std::printf("#%zu qpc=%llu dwData=%llu cbData=%u flags=%u%s\n", ctx->index,
              static_cast<unsigned long long>(hdr->qpc),
              static_cast<unsigned long long>(hdr->dwData), hdr->cbData, hdr->flags,
              (hdr->flags & kFlagTruncated) ? " TRUNCATED" : "");
  for (uint32_t off = 0; off < hdr->cbData; off += 8) {
    const uint32_t n = (hdr->cbData - off < 8) ? hdr->cbData - off : 8;
    const uint8_t* p = payload + off;
    std::printf("  +%04X ", off);
    for (uint32_t i = 0; i < 8; ++i) {
      if (i < n) {
        std::printf("%02X ", p[i]);
      } else {
        std::printf("   ");
      }
    }
    if (n == 8) {
      std::printf(" u32[0]=0x%08X u32[1]=0x%08X u64=0x%016llX ", RdU32(p), RdU32(p + 4),
                  static_cast<unsigned long long>(RdU64(p)));
    } else {
      std::printf("%*s", 44, " ");
    }
    for (uint32_t i = 0; i < n; ++i) {
      std::printf("%c", (p[i] >= 0x20 && p[i] < 0x7F) ? static_cast<char>(p[i]) : '.');
    }
    std::printf("\n");
  }
  ++ctx->index;
  return true;
}

// --- stats ----------------------------------------------------------------------

struct StatsCtx {
  size_t total = 0;
  size_t lost = 0;
  size_t truncated = 0;
  std::map<uint64_t, size_t> byDwData;
  std::map<uint32_t, size_t> cbDataForTray;  // dwData == 1 only
};

bool StatsSink(const SpikeRecordHeader* hdr, const uint8_t* payload, void* user) {
  auto* st = static_cast<StatsCtx*>(user);
  (void)payload;
  if (hdr == nullptr) {
    ++st->lost;
    return true;
  }
  ++st->total;
  ++st->byDwData[hdr->dwData];
  if ((hdr->flags & kFlagTruncated) != 0) ++st->truncated;
  if (hdr->dwData == 1) ++st->cbDataForTray[hdr->cbData];
  return true;
}

int PrintUsage() {
  std::printf(
      "usage: TraySpikeDump [--in <path>] [--mode words|stats] [--limit N]\n"
      "  default --in: %%TEMP%%\\TrayMirrorSpike.log, mode: words\n");
  return 1;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  std::wstring path;
  wchar_t defaultPath[MAX_PATH];
  if (GetSpikeLogPathW(defaultPath, MAX_PATH)) path = defaultPath;
  std::wstring mode = L"words";
  size_t limit = 0;

  for (int i = 1; i < argc; ++i) {
    const std::wstring a(argv[i]);
    if (a == L"--in" && i + 1 < argc) {
      path = argv[++i];
    } else if (a == L"--mode" && i + 1 < argc) {
      mode = argv[++i];
    } else if (a == L"--limit" && i + 1 < argc) {
      limit = static_cast<size_t>(wcstoul(argv[++i], nullptr, 10));
    } else {
      return PrintUsage();
    }
  }
  if (mode != L"words" && mode != L"stats") return PrintUsage();

  std::vector<uint8_t> data = ReadAllBytes(path);
  if (data.empty() && (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)) {
    std::printf("cannot read log: %ls\n", path.c_str());
    return 1;
  }

  size_t skipped = 0;
  if (mode == L"words") {
    WordsCtx ctx;
    ctx.limit = limit;
    const size_t n = ParseRecords(data.data(), data.size(), WordsSink, &ctx, &skipped);
    std::printf("--- records shown: %zu, skipped garbage bytes: %zu\n", n, skipped);
  } else {
    StatsCtx st;
    ParseRecords(data.data(), data.size(), StatsSink, &st, &skipped);
    std::printf("total records: %zu\n", st.total);
    std::printf("sync-lost tails: %zu\n", st.lost);
    std::printf("truncated: %zu\n", st.truncated);
    std::printf("by dwData:\n");
    for (const auto& kv : st.byDwData) {
      std::printf("  dwData=%llu count=%zu\n", static_cast<unsigned long long>(kv.first),
                  kv.second);
    }
    std::printf("cbData distribution for dwData=1:\n");
    for (const auto& kv : st.cbDataForTray) {
      std::printf("  cbData=%u count=%zu\n", kv.first, kv.second);
    }
    std::printf("skipped garbage bytes: %zu\n", skipped);
  }
  return 0;
}
