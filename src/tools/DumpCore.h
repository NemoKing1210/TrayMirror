// Snapshot parser for the diagnostics dump tool (spec 2.2, plan task
// 9). A pure function over a PRIVATE copy of the mapping — shared
// memory is never parsed in place: a race with the live writer can only
// produce garbage records that validation discards.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

struct DumpOptions {
  bool summaryOnly = false;  // --mode summary
  int limit = 0;             // --limit N; 0 = unlimited
};

// Parses one whole-mapping snapshot (RingHeader followed by ringSize
// bytes) and prints decoded records plus a trailer to `out`. Advances
// readPos inside the SNAPSHOT only — the live ring is untouched.
// Returns the number of tray records found, or -1 when the snapshot is
// not a TrayMirror ring.
int DumpSnapshot(const uint8_t* snap, size_t snapSize,
                 const DumpOptions& opt, FILE* out);
