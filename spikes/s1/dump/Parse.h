#pragma once
#include "Record.h"

// Parses the spike log record stream with resync by magic (plan, task 7).
// sink(nullptr, nullptr, user) — "sync lost" signal: magic found, but the record
// is incomplete (torn write). Returning false from sink stops parsing.
using RecordSinkFn = bool (*)(const SpikeRecordHeader* hdr, const uint8_t* payload,
                               void* user);

// Returns the number of complete valid records passed to sink.
// *skipped (if not nullptr) — total garbage bytes skipped during resync.
size_t ParseRecords(const uint8_t* data, size_t size, RecordSinkFn sink, void* user,
                    size_t* skipped);
