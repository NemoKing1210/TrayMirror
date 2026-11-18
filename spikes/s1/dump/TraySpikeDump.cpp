// TraySpikeDump — spike log record dump/statistics (plan, task 7).
#include "Layout.h"
#include "Parse.h"
#include "Record.h"
#include "TraySpike.h"

#include <clocale>
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

// --- decode: interpret records with the MEASURED tray layout (Layout.h) ------

struct DecodeCtx {
  size_t index = 0;
  size_t limit = 0;  // 0 = no limit
  size_t decoded = 0;
  size_t nonTray = 0;
};

const char* NimName(uint32_t op) {
  switch (op) {
    case 0: return "NIM_ADD";
    case 1: return "NIM_MODIFY";
    case 2: return "NIM_DELETE";
    case 3: return "NIM_SETFOCUS";
    case 4: return "NIM_SETVERSION";
    default: return "NIM_?";
  }
}

void PrintNifFlags(uint32_t f) {
  if (f & 0x1) std::printf(" NIF_MESSAGE");
  if (f & 0x2) std::printf(" NIF_ICON");
  if (f & 0x4) std::printf(" NIF_TIP");
  if (f & 0x8) std::printf(" NIF_STATE");
  if (f & 0x10) std::printf(" NIF_INFO");
  if (f & 0x20) std::printf(" NIF_GUID");
  if (f & 0x40) std::printf(" NIF_REALTIME");
  if (f & 0x80) std::printf(" NIF_SHOWTIP");
  if (f & 0x100) std::printf(" NIF_STATICITEM");
}

bool DecodeSink(const SpikeRecordHeader* hdr, const uint8_t* payload, void* user) {
  auto* ctx = static_cast<DecodeCtx*>(user);
  if (hdr == nullptr) {
    std::printf("  [sync lost: torn record]\n");
    return true;
  }
  if (ctx->limit != 0 && ctx->index >= ctx->limit) return false;
  ++ctx->index;

  // Never decode a non-tray subtype as tray (plan Review Focus).
  if (hdr->dwData != 1 || hdr->cbData < kTrayPayloadSize ||
      RdU32(payload + 0) != kTrayMsgMagic) {
    ++ctx->nonTray;
    std::printf("#%zu [non-tray] dwData=%llu cbData=%u\n", ctx->index - 1,
                static_cast<unsigned long long>(hdr->dwData), hdr->cbData);
    return true;
  }

  const uint32_t op = RdU32(payload + kOffNim);
  const uint32_t hwnd = RdU32(payload + kOffHwnd);
  const uint32_t uid = RdU32(payload + kOffUid);
  const uint32_t flags = RdU32(payload + kOffFlags);
  const uint32_t cbmsg = RdU32(payload + kOffCallbackMsg);
  const uint32_t icon = RdU32(payload + kOffIcon);
  wchar_t tip[130] = {};  // worst case: 64 wchars, each escaped to two
  size_t o = 0;
  for (size_t i = 0; i < 64 && kOffTip + i * 2 + 1 < hdr->cbData && o + 2 < 130; ++i) {
    const wchar_t ch = static_cast<wchar_t>(payload[kOffTip + i * 2] |
                                            (payload[kOffTip + i * 2 + 1] << 8));
    if (ch == L'\0') break;  // the tail after NUL is stale buffer bytes
    if (ch == L'\r') {
      tip[o++] = L'\\';
      tip[o++] = L'r';
    } else if (ch == L'\n') {
      tip[o++] = L'\\';
      tip[o++] = L'n';
    } else if (ch < 0x20) {
      tip[o++] = L'.';  // any other control char is noise
    } else {
      tip[o++] = ch;
    }
  }

  ++ctx->decoded;
  std::printf("#%zu %s hwnd=0x%X uid=%u flags=0x%X", ctx->index - 1, NimName(op),
              hwnd, uid, flags);
  PrintNifFlags(flags);
  std::printf(" callback=0x%X icon=0x%X tip=\"%ls\"\n", cbmsg, icon, tip);
  return true;
}

int PrintUsage() {
  std::printf(
      "usage: TraySpikeDump [--in <path>] [--mode words|stats|decode] [--limit N]\n"
      "  default --in: %%TEMP%%\\TrayMirrorSpike.log, mode: words\n");
  return 1;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  // printf("%ls") converts wide chars through the current locale. In the
  // default "C" locale MSVC cannot convert non-ASCII and silently drops the
  // remainder of the formatted output; UTF-8 output keeps real tips intact.
  std::setlocale(LC_ALL, ".UTF8");
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
  if (mode != L"words" && mode != L"stats" && mode != L"decode") return PrintUsage();

  std::vector<uint8_t> data = ReadAllBytes(path);
  if (data.empty() && (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)) {
    std::printf("cannot read log: %ls\n", path.c_str());
    return 1;
  }

  size_t skipped = 0;
  if (mode == L"decode") {
    DecodeCtx ctx;
    ctx.limit = limit;
    ParseRecords(data.data(), data.size(), DecodeSink, &ctx, &skipped);
    std::printf("--- decoded tray: %zu, non-tray: %zu, skipped garbage: %zu\n",
                ctx.decoded, ctx.nonTray, skipped);
  } else if (mode == L"words") {
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
