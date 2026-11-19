#include "ReaderThread.h"

#include "Log.h"
#include "RecordCodec.h"
#include "RingReader.h"
#include "TrayPayload.h"

#include <vector>

void ReaderLoop(volatile RingHeader* header, volatile uint8_t* ring,
                HANDLE signal, HANDLE stop, const RecordSink& sink) {
  auto log = InitHostLogging("");
  std::vector<uint8_t> buf(kRecordHeaderSize + kMaxPayloadBytes +
                           kRecordFooterSize);
  std::vector<uint8_t> scratch(header->ringSize);
  uint32_t droppedSeen = 0;

  HANDLE waits[2] = {stop, signal};
  for (;;) {
    const DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
    if (w == WAIT_OBJECT_0) {
      return;  // stop: the ring is left to the next host session
    }

    for (;;) {
      RingRecordHeader hdr = {};
      const uint8_t* payload = nullptr;
      const RingReadStatus st = RingReaderGet(header, ring, buf.data(),
                                              static_cast<uint32_t>(buf.size()),
                                              &hdr, &payload);
      if (st == RingReadStatus::Ok) {
        TrayMessage msg = {};
        const TrayDecodeStatus ds = DecodeTrayPayload(payload, hdr.payloadSize, &msg);
        if (ds == TrayDecodeStatus::Ok) {
          sink(hdr, msg);
        } else if (ds == TrayDecodeStatus::NotTray) {
          log->debug("non-tray record type={} dropped silently", hdr.type);
        } else {
          log->warn("record type={} payload {} bytes failed tray decode",
                    hdr.type, hdr.payloadSize);
        }
        continue;
      }
      if (st == RingReadStatus::Torn) {
        const uint64_t newPos = RingReaderResync(header, ring, scratch.data(),
                                                 scratch.size());
        log->warn("torn record; resynchronized to pos {}", newPos);
        continue;
      }
      if (st == RingReadStatus::Invalid) {
        log->error("ring mapping is not ours (magic/version) — stopping");
        return;
      }
      break;  // Empty
    }

    const uint32_t dropped = header->droppedCount;
    if (dropped != droppedSeen) {
      log->warn("ring overflow: {} records dropped (resync is stage 2)",
                dropped - droppedSeen);
      droppedSeen = dropped;
    }
  }
}
