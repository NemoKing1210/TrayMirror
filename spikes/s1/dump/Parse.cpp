#include "Parse.h"

// TDD GREEN step (plan, task 7).
// Resync: bad magic/size — advance one garbage byte, bump the skipped counter.
// Torn write: magic found but the record is incomplete — sink(nullptr) and end of stream.

size_t ParseRecords(const uint8_t* data, size_t size, RecordSinkFn sink, void* user,
                    size_t* skipped) {
  if (skipped != nullptr) *skipped = 0;
  if (data == nullptr || sink == nullptr) return 0;

  size_t count = 0;
  size_t skip = 0;
  size_t pos = 0;

  while (pos < size) {
    const size_t remaining = size - pos;
    if (remaining < sizeof(uint32_t)) break;  // magic can no longer be checked

    uint32_t magic;
    memcpy(&magic, data + pos, sizeof(magic));
    if (magic != kSpikeMagic) {
      ++skip;
      ++pos;
      continue;
    }

    if (remaining < sizeof(SpikeRecordHeader)) {
      // torn write: magic present, header incomplete
      if (!sink(nullptr, nullptr, user)) break;
      break;
    }

    SpikeRecordHeader h;
    memcpy(&h, data + pos, sizeof(h));
    if (h.totalSize < sizeof(SpikeRecordHeader) ||
        h.totalSize > sizeof(SpikeRecordHeader) + kSpikeMaxPayload ||
        h.cbData != h.totalSize - sizeof(SpikeRecordHeader)) {
      // False magic or inconsistent size: treat as garbage, resync.
      ++skip;
      ++pos;
      continue;
    }
    if (pos + h.totalSize > size) {
      // torn write: the record started, payload truncated
      if (!sink(nullptr, nullptr, user)) break;
      break;
    }

    ++count;
    if (!sink(&h, data + pos + sizeof(h), user)) break;
    pos += h.totalSize;
  }

  if (skipped != nullptr) *skipped = skip;
  return count;
}
