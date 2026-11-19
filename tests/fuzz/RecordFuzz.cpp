// Deterministic fuzz corpus for the host record parsing path (plan
// task 10). Everything a hostile or torn writer can put into the ring
// goes through the same byte path the host uses:
// ValidateRingRecord, FindNextRecord, RingReaderGet, DecodeTrayPayload.
//
// No SEH anywhere in this harness: the parser must survive garbage by
// construction (every read is bounds-checked against a caller-supplied
// cap), so a crash here is a defect, not an input problem.
//
// The seed is fixed: every run exercises the identical corpus.
#include "Proto.h"
#include "RecordCodec.h"
#include "Ring.h"
#include "RingReader.h"
#include "TrayPayload.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr uint64_t kSeed = 0x853c49e6748fea9bULL;
constexpr int kBuffers = 200000;
constexpr size_t kMaxFuzzBuf = kRingSizeBytes / 16;

uint64_t XorShift64(uint64_t* s) {
  uint64_t x = *s;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  *s = x;
  return x;
}

// Edge-biased size: a quarter of the picks land on known boundaries
// (record header, payload cap, the measured tray sizes), the rest are
// uniform in [0, kMaxFuzzBuf).
size_t PickSize(uint64_t* s) {
  static constexpr size_t kEdges[] = {
      0,  1,  2,  3,  7,  8,  9,  23, 24, 25, 27, 28, 29,
      31, 32, 39, 40, 41, 1483, 1484, 1491, 1492, 1520,
      kRecordHeaderSize + kMaxPayloadBytes + kRecordFooterSize};
  if (XorShift64(s) % 4 == 0) {
    return kEdges[XorShift64(s) % (sizeof(kEdges) / sizeof(kEdges[0]))];
  }
  return static_cast<size_t>(XorShift64(s) % kMaxFuzzBuf);
}

// A fully valid tray record (mini-header + measured TRAYDATA).
std::vector<uint8_t> MakeValidTrayRecord(const wchar_t* tip) {
  std::vector<uint8_t> payload(8 + kTrayPayloadSize, 0);
  const uint32_t dw = 1;
  const uint32_t cb = kTrayPayloadSize;
  const uint32_t magic = kTrayMsgMagic;
  memcpy(payload.data(), &dw, 4);
  memcpy(payload.data() + 4, &cb, 4);
  memcpy(payload.data() + 8, &magic, 4);
  memcpy(payload.data() + 8 + kTrayOffTip, tip, (wcslen(tip) + 1) * sizeof(wchar_t));
  std::vector<uint8_t> rec(
      kRecordHeaderSize + payload.size() + kRecordFooterSize);
  const size_t n = PackRingRecord(rec.data(), rec.size(), kRecNotifyCopydata,
                                  123, 77, payload.data(),
                                  static_cast<uint32_t>(payload.size()));
  rec.resize(n);
  return rec;
}

int g_failures = 0;

void Fail(const char* what) {
  std::printf("FUZZ FAIL: %s\n", what);
  ++g_failures;
}

// A minimal valid record: payload is the mini-header only (dwData == 1,
// cbData == 0). Small on purpose — the scan-heavy corpus cases splice
// this one into garbage windows without paying for a 1.5 KB tray
// payload at every step.
std::vector<uint8_t> MakeMiniRecord() {
  uint8_t payload[8] = {};
  const uint32_t dw = 1;
  memcpy(payload, &dw, sizeof(dw));
  std::vector<uint8_t> rec(
      kRecordHeaderSize + sizeof(payload) + kRecordFooterSize);
  const size_t n = PackRingRecord(
      rec.data(), rec.size(), kRecNotifyCopydata, 5, 6, payload,
      static_cast<uint32_t>(sizeof(payload)));
  rec.resize(n);
  return rec;
}

// --- hand-injected evil cases: the harness's own negative controls.
// They pin the discriminating power the corpus relies on. ------------

void NegativeControls() {
  // (1) A record with a corrupted CRC must not validate, and
  // FindNextRecord must not return its offset.
  {
    std::vector<uint8_t> rec = MakeValidTrayRecord(L"crc-broken");
    rec.back() ^= 0xFF;
    RingRecordHeader hdr = {};
    const uint8_t* payload = nullptr;
    if (ValidateRingRecord(rec.data(), rec.size(), &hdr, &payload)) {
      Fail("corrupted CRC validated");
    }
    const size_t off =
        FindNextRecord(rec.data(), rec.size(), 0, &hdr);
    if (off != static_cast<size_t>(SIZE_MAX) &&
        ValidateRingRecord(rec.data() + off, rec.size() - off, &hdr,
                           &payload)) {
      // An offset may be *found* only if a different, valid record
      // starts there; in a single-record buffer that means the
      // discriminator failed.
      Fail("FindNextRecord accepted a CRC-broken record");
    }
  }

  // (2) A lying cbData (mini-header claims 0xFFFFFFFF, payload carries
  // 16 bytes) must decode as Invalid — never read past payloadSize.
  {
    std::vector<uint8_t> payload(8 + 16, 0);
    const uint32_t dw = 1;
    const uint32_t lie = 0xFFFFFFFF;
    const uint32_t magic = kTrayMsgMagic;
    memcpy(payload.data(), &dw, 4);
    memcpy(payload.data() + 4, &lie, 4);
    memcpy(payload.data() + 8, &magic, 4);
    TrayMessage msg = {};
    const TrayDecodeStatus st =
        DecodeTrayPayload(payload.data(), static_cast<uint32_t>(payload.size()), &msg);
    if (st != TrayDecodeStatus::Invalid) {
      Fail("cbData lie was not rejected as Invalid");
    }
  }

  // (3) An incoherent record header (size smaller than
  // header + payload + CRC) must not validate.
  {
    std::vector<uint8_t> rec = MakeValidTrayRecord(L"size-lie");
    RingRecordHeader* rh = reinterpret_cast<RingRecordHeader*>(rec.data());
    rh->size = kRecordHeaderSize;  // claims header-only, carries more
    RingRecordHeader hdr = {};
    const uint8_t* payload = nullptr;
    if (ValidateRingRecord(rec.data(), rec.size(), &hdr, &payload)) {
      Fail("incoherent record size validated");
    }
  }
}

}  // namespace

int main() {
  NegativeControls();
  if (g_failures != 0) {
    std::printf("FUZZ FAILED: negative controls\n");
    return 1;
  }
  std::printf("negative controls: 3/3 held\n");

  uint64_t s = kSeed;
  long long spliced = 0;
  long long torn = 0;
  long long lies = 0;

  std::vector<uint8_t> buf(kMaxFuzzBuf + 64);
  std::vector<uint8_t> readerBuf(kRecordHeaderSize + kMaxPayloadBytes +
                                 kRecordFooterSize);
  std::vector<uint8_t> scratch(kRingSizeBytes);

  const std::vector<uint8_t> validRec = MakeValidTrayRecord(L"fuzz-splice");
  const std::vector<uint8_t> miniRec = MakeMiniRecord();

  for (int i = 0; i < kBuffers; ++i) {
    const size_t size = PickSize(&s);
    // Word-at-a-time fill: per-byte xorshift over ~64 KB buffers was the
    // corpus's own hot spot. The stream stays deterministic.
    {
      size_t j = 0;
      for (; j + 8 <= size; j += 8) {
        const uint64_t v = XorShift64(&s);
        memcpy(buf.data() + j, &v, sizeof(v));
      }
      for (; j < size; ++j) {
        buf[j] = static_cast<uint8_t>(XorShift64(&s));
      }
    }
    // Bias some buffers toward plausible-looking but lying content.
    if (size >= 12 && (i & 3) == 0) {
      const uint32_t lie = 0xFFFFFFFF;
      memcpy(buf.data() + 4, &lie, 4);
    }

    // (a) validate must not crash and must stay inside the buffer;
    // (b) the decoder must answer with an enum value on the same bytes.
    {
      RingRecordHeader hdr = {};
      const uint8_t* payload = nullptr;
      (void)ValidateRingRecord(buf.data(), size, &hdr, &payload);
      TrayMessage msg = {};
      const TrayDecodeStatus ds =
          DecodeTrayPayload(buf.data(), static_cast<uint32_t>(size), &msg);
      if (ds != TrayDecodeStatus::NotTray && ds != TrayDecodeStatus::Ok &&
          ds != TrayDecodeStatus::Invalid) {
        Fail("decoder returned a value outside its enum");
      }
    }

    // Scan-heavy cases (FindNextRecord walks candidate offsets; the
    // reader path can resync) run on a small window: the invariant is
    // about the discriminator's behavior, and a 1 KB haystack proves it
    // as well as a 64 KB one while keeping the whole corpus well under
    // the 30 s plan budget. Pure validate/decode keep full sizes.
    const size_t scanCap = size > 1024 ? 1024 : size;

    // (c) a whole valid record spliced into garbage must be found.
    if ((i % 7) == 0 && scanCap >= miniRec.size() + 1) {
      const size_t at = 1;  // garbage prefix, record, garbage suffix
      memcpy(buf.data() + at, miniRec.data(), miniRec.size());
      RingRecordHeader hdr = {};
      const size_t off = FindNextRecord(buf.data(), scanCap, 0, &hdr);
      if (off != at) {
        Fail("spliced valid record not found at its offset");
      } else {
        const uint8_t* payload = nullptr;
        if (!ValidateRingRecord(buf.data() + off, scanCap - off, &hdr,
                                &payload)) {
          Fail("found record did not validate");
        }
      }
      ++spliced;
    }
    // (c2) the same invariant at full tray scale, sampled sparsely so
    // the 1.5 KB payload does not dominate the corpus time; the found
    // record must also decode Ok.
    if ((i % 1009) == 0) {
      memcpy(buf.data(), validRec.data(), validRec.size());
      buf[validRec.size()] = static_cast<uint8_t>(XorShift64(&s));
      RingRecordHeader hdr = {};
      const size_t off =
          FindNextRecord(buf.data(), validRec.size() + 1, 0, &hdr);
      if (off != 0) {
        Fail("full-size spliced valid record not found");
      } else {
        const uint8_t* payload = nullptr;
        if (!ValidateRingRecord(buf.data(), validRec.size() + 1, &hdr,
                                &payload)) {
          Fail("full-size found record did not validate");
        } else {
          TrayMessage msg = {};
          if (DecodeTrayPayload(payload, hdr.payloadSize, &msg) !=
              TrayDecodeStatus::Ok) {
            Fail("full-size spliced tray record did not decode Ok");
          }
        }
      }
      ++spliced;
    }

    // (d) a torn record (byte flipped mid-payload) must never be
    // returned as found.
    if ((i % 13) == 0 && scanCap >= miniRec.size() + 1) {
      memcpy(buf.data() + 1, miniRec.data(), miniRec.size());
      buf[1 + kRecordHeaderSize + 4] ^= 0xFF;  // inside the payload
      RingRecordHeader hdr = {};
      const size_t off = FindNextRecord(buf.data(), scanCap, 0, &hdr);
      if (off == static_cast<size_t>(1)) {
        Fail("torn record returned as valid");
      }
      ++torn;
    }

    // (e) the full reader path over garbage with random positions:
    // the status must be an enum value, never a crash. The window is
    // smaller still — resync scans dominate the corpus time.
    {
      size_t rSize = size;
      if (rSize > 512) {
        rSize = 512;
      }
      if (rSize < 2 * (kRecordHeaderSize + kRecordFooterSize)) {
        rSize = 2 * (kRecordHeaderSize + kRecordFooterSize);
      }
      if ((i % 5) == 0) {
        RingHeader h = {};
        h.magic = kRingMagic;
        h.protoVersion = kProtoVersion;
        h.headerSize = sizeof(RingHeader);
        h.ringSize = static_cast<uint32_t>(rSize);
        const uint64_t w = XorShift64(&s) % rSize;
        const uint64_t r = XorShift64(&s) % (rSize + 1);
        h.writePos = w;
        h.readPos = r > w ? w : r;  // reader never leads the writer
        RingRecordHeader hdr = {};
        const uint8_t* payload = nullptr;
        const RingReadStatus st = RingReaderGet(
            &h, buf.data(), readerBuf.data(),
            static_cast<uint32_t>(readerBuf.size()), &hdr, &payload);
        if (st == RingReadStatus::Torn) {
          // resync must also survive the same garbage.
          RingReaderResync(&h, buf.data(), scratch.data(), scratch.size());
        }
      }
    }

    // (f) the lying cbData inside a plausible tray payload.
    if ((i % 11) == 0 && size >= 8 + 32) {
      const uint32_t magic = kTrayMsgMagic;
      memcpy(buf.data() + 8, &magic, 4);
      TrayMessage msg = {};
      const TrayDecodeStatus ds = DecodeTrayPayload(
          buf.data(), static_cast<uint32_t>(size), &msg);
      if (ds == TrayDecodeStatus::Ok && !msg.truncated &&
          size < 8 + kTrayPayloadSize) {
        Fail("short payload with lying cbData decoded as complete");
      }
      ++lies;
    }
  }

  if (g_failures != 0) {
    std::printf("FUZZ FAILED: %d invariant breaches\n", g_failures);
    return 1;
  }
  std::printf(
      "FUZZ PASSED: %d buffers, %lld spliced-found, %lld torn-rejected, "
      "%lld cbData-lies\n",
      kBuffers, spliced, torn, lies);
  return 0;
}
