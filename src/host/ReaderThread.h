// The reader thread (spec 2.4): waits on the signal event, copies
// records out of the ring, decodes the tray payload, hands each record
// to the sink. Torn records resynchronize; a foreign mapping (Invalid)
// stops the loop — the host refuses to guess (spec 2.3.4).
#pragma once

#include "Ring.h"
#include "TrayPayload.h"

#include <windows.h>

#include <cstdint>
#include <functional>

// Sink receives (record header, fully decoded tray message). Runs on
// the reader thread.
using RecordSink =
    std::function<void(const RingRecordHeader&, const TrayMessage&)>;

// Returns when the stop event is signaled and the ring is drained.
// `resync` (optional) is the manual-resync event; `onResync` (optional)
// is invoked on overflow and when `resync` is signaled. Both are host
// plumbing (spec 4.3); onResync must not block the reader.
void ReaderLoop(volatile RingHeader* header, volatile uint8_t* ring,
                HANDLE signal, HANDLE stop, const RecordSink& sink,
                HANDLE resync = nullptr,
                const std::function<void()>& onResync = {});
