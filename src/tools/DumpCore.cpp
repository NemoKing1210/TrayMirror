#include "DumpCore.h"

#include "Proto.h"
#include "Ring.h"
#include "RingReader.h"
#include "TrayPayload.h"

#include <vector>

int DumpSnapshot(const uint8_t* snap, size_t snapSize,
                 const DumpOptions& opt, FILE* out) {
  if (snapSize < sizeof(RingHeader)) {
    std::fprintf(out, "not a TrayMirror ring (snapshot too small)\n");
    return -1;
  }
  RingHeader h = {};
  memcpy(&h, snap, sizeof(h));
  if (h.magic != kRingMagic || h.protoVersion != kProtoVersion ||
      h.headerSize != sizeof(RingHeader)) {
    std::fprintf(out, "not a TrayMirror ring (magic/version mismatch)\n");
    return -1;
  }
  constexpr auto kMinRecordBytes =
      static_cast<uint32_t>(kRecordHeaderSize + kRecordFooterSize);
  if (h.ringSize < 2 * kMinRecordBytes ||
      snapSize < sizeof(h) + static_cast<size_t>(h.ringSize)) {
    std::fprintf(out, "not a TrayMirror ring (bad ring size)\n");
    return -1;
  }

  // The snapshot is private memory: the reader may advance readPos in
  // it freely. The live ring's readPos is never touched.
  auto* header =
      reinterpret_cast<volatile RingHeader*>(const_cast<uint8_t*>(snap));
  auto* ring = const_cast<uint8_t*>(snap) + h.headerSize;

  std::vector<uint8_t> buf(kRecordHeaderSize + kMaxPayloadBytes +
                           kRecordFooterSize);
  std::vector<uint8_t> scratch(h.ringSize);

  int printed = 0;
  int records = 0;
  int skipped = 0;
  for (;;) {
    if (opt.limit != 0 && printed >= opt.limit) {
      break;
    }
    RingRecordHeader rh = {};
    const uint8_t* payload = nullptr;
    const RingReadStatus st =
        RingReaderGet(header, ring, buf.data(),
                      static_cast<uint32_t>(buf.size()), &rh, &payload);
    if (st == RingReadStatus::Empty) {
      break;
    }
    if (st == RingReadStatus::Torn) {
      // Garbage from a writer race: resync and move on. This is the
      // passive-observer answer to torn data — discard, never crash.
      RingReaderResync(header, ring, scratch.data(), scratch.size());
      ++skipped;
      continue;
    }
    if (st == RingReadStatus::Invalid) {
      // Cannot happen on a private snapshot validated above, but the
      // invariant is cheap to keep: refuse foreign memory, stop.
      std::fprintf(out, "not a TrayMirror ring (mapping changed mid-parse)\n");
      break;
    }

    TrayMessage msg = {};
    const TrayDecodeStatus ds = DecodeTrayPayload(payload, rh.payloadSize, &msg);
    if (ds == TrayDecodeStatus::Ok) {
      if (!opt.summaryOnly) {
        std::fprintf(out,
                     "[%d] type=%u nim=%u hwnd=0x%08X uid=0x%08X tip=\"%ls\"%s\n",
                     printed, rh.type, msg.nim, msg.hwnd, msg.uid, msg.tip,
                     msg.truncated ? " (truncated)" : "");
        ++printed;
      }
      ++records;
    } else if (ds == TrayDecodeStatus::NotTray) {
      // A non-tray record: same policy as the host reader (spec 3.3) —
      // ignore silently.
      continue;
    } else {
      if (!opt.summaryOnly) {
        std::fprintf(out, "[%d] type=%u payload=%u not-decodable\n", printed,
                     rh.type, rh.payloadSize);
        ++printed;
      }
      ++skipped;
    }
  }

  std::fprintf(out, "records=%d skipped=%d dropped=%u hookAlive=%u explorerPid=%u\n",
               records, skipped, h.droppedCount, h.hookAlive, h.explorerPid);
  return records;
}
