// TDD test for parsing the spike log record stream (plan, task 7).
// Run: ctest -R DumpTests --output-on-failure
#include "Parse.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

struct SinkState {
  int valid = 0;      // complete records
  int lost = 0;       // sync-lost signals
  bool seenTruncated = false;
  bool stopEarly = false;  // if true, sink returns false after the first record
};

bool Sink(const SpikeRecordHeader* hdr, const uint8_t* payload, void* user) {
  auto* st = static_cast<SinkState*>(user);
  if (hdr == nullptr) {
    ++st->lost;
    return true;
  }
  if (hdr->flags == kFlagTruncated) st->seenTruncated = true;
  (void)payload;
  ++st->valid;
  return !st->stopEarly;
}

bool BuildRecords(std::vector<uint8_t>* out, int n) {
  for (int i = 0; i < n; ++i) {
    const uint8_t payload[4] = {1, 2, 3, 4};
    uint8_t rec[64] = {};
    const size_t len = PackSpikeRecord(rec, sizeof(rec), 1, 1, payload, 4);
    if (len == 0) return false;
    out->insert(out->end(), rec, rec + len);
  }
  return true;
}

int g_failures = 0;
void Check(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    ++g_failures;
  }
}

}  // namespace

int main() {
  // (a) Three valid records → sink three times, 0 lost, 0 skipped.
  {
    std::vector<uint8_t> data;
    Check(BuildRecords(&data, 3), "(a) build");
    SinkState st;
    size_t skipped = 0xDEAD;
    const size_t n = ParseRecords(data.data(), data.size(), Sink, &st, &skipped);
    Check(n == 3 && st.valid == 3 && st.lost == 0, "(a) 3 valid records");
    Check(skipped == 0, "(a) nothing skipped");
  }

  // (b) Torn write: the record is cut in the middle → 2 valid + 1 lost.
  {
    std::vector<uint8_t> data;
    Check(BuildRecords(&data, 3), "(b) build");
    data.resize(data.size() - 20);  // cut the tail of the third record (36 -> 16 bytes)
    SinkState st;
    size_t skipped = 0;
    const size_t n = ParseRecords(data.data(), data.size(), Sink, &st, &skipped);
    Check(n == 2 && st.valid == 2, "(b) 2 full records before torn tail");
    Check(st.lost == 1, "(b) torn tail signaled as sync lost");
    Check(skipped == 0, "(b) no garbage skipped");
  }

  // (c) Garbage before the first record → resync by magic, skipped == number of garbage bytes.
  {
    const uint8_t garbage[8] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
    std::vector<uint8_t> data(garbage, garbage + 8);
    Check(BuildRecords(&data, 1), "(c) build");
    SinkState st;
    size_t skipped = 0;
    const size_t n = ParseRecords(data.data(), data.size(), Sink, &st, &skipped);
    Check(n == 1 && st.valid == 1 && st.lost == 0, "(c) resync finds record after garbage");
    Check(skipped == 8, "(c) skipped == 8 garbage bytes");
  }

  // (d) Empty buffer → 0 records, sink is not called.
  {
    SinkState st;
    size_t skipped = 0;
    const size_t n = ParseRecords(nullptr, 0, Sink, &st, &skipped);
    Check(n == 0 && st.valid == 0 && st.lost == 0 && skipped == 0, "(d) empty buffer");
  }

  // (e) A record with kFlagTruncated and cbData==4096 passes as valid.
  {
    static uint8_t big[kSpikeMaxPayload + 100] = {};
    uint8_t rec[sizeof(SpikeRecordHeader) + kSpikeMaxPayload] = {};
    const size_t len = PackSpikeRecord(rec, sizeof(rec), 9, 1, big, kSpikeMaxPayload + 100);
    Check(len == sizeof(rec), "(e) build");
    SinkState st;
    size_t skipped = 0;
    ParseRecords(rec, len, Sink, &st, &skipped);
    Check(st.valid == 1 && st.seenTruncated, "(e) truncated record parsed");
  }

  // (f) sink returned false after the first record → parsing stopped, no records lost.
  {
    std::vector<uint8_t> data;
    Check(BuildRecords(&data, 2), "(f) build");
    SinkState st;
    st.stopEarly = true;
    const size_t n = ParseRecords(data.data(), data.size(), Sink, &st, nullptr);
    Check(n == 1 && st.valid == 1, "(f) early stop honored");
  }

  if (g_failures != 0) {
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
