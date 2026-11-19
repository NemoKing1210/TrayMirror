// TrayMirrorDump — passive ring observer (spec 2.2, plan task 9).
// Opens the mapping read-only, takes ONE memcpy snapshot of the whole
// mapping, and parses the private copy. Never contacts Explorer and
// never writes the live ring's readPos.
#include "DumpCore.h"

#include "Proto.h"
#include "Ring.h"
#include "ipc/SharedRing.h"

#include <windows.h>

#include <clocale>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

int wmain(int argc, wchar_t** argv) {
  // Spike S2 lesson: printf("%ls") drops the entire remainder of the
  // formatted output when a wide char is not convertible in the C
  // locale; tray tips are UTF-16 and must print as UTF-8.
  setlocale(LC_ALL, ".UTF8");

  DumpOptions opt;
  for (int i = 1; i < argc; ++i) {
    if (wcscmp(argv[i], L"--mode") == 0 && i + 1 < argc) {
      ++i;
      if (wcscmp(argv[i], L"records") == 0) {
        opt.summaryOnly = false;
      } else if (wcscmp(argv[i], L"summary") == 0) {
        opt.summaryOnly = true;
      } else {
        std::printf("usage: TrayMirrorDump [--mode records|summary] [--limit N]\n");
        return 1;
      }
    } else if (wcscmp(argv[i], L"--limit") == 0 && i + 1 < argc) {
      opt.limit = static_cast<int>(wcstol(argv[++i], nullptr, 10));
      if (opt.limit <= 0) {
        std::printf("usage: TrayMirrorDump [--mode records|summary] [--limit N]\n");
        return 1;
      }
    } else {
      std::printf("usage: TrayMirrorDump [--mode records|summary] [--limit N]\n");
      return 1;
    }
  }

  SharedRingHandle ring = {};
  if (!SharedRingOpenRead(&ring)) {
    std::printf("TrayMirror ring not found (is the host running?)\n");
    return 3;
  }

  const uint32_t ringSize = ring.header->ringSize;
  constexpr auto kMinRecordBytes =
      static_cast<uint32_t>(kRecordHeaderSize + kRecordFooterSize);
  if (ringSize < 2 * kMinRecordBytes || ringSize > kRingSizeBytes) {
    std::printf("ring reports an implausible size %u - refusing\n", ringSize);
    SharedRingClose(&ring);
    return 4;
  }

  // One snapshot of the whole mapping: header + ring in a single copy.
  // A concurrent writer can tear the copy; validation discards the
  // garbage records and resyncs inside the private bytes.
  const size_t snapSize = sizeof(RingHeader) + static_cast<size_t>(ringSize);
  std::vector<uint8_t> snap(snapSize);
  memcpy(snap.data(), ring.view, snapSize);
  SharedRingClose(&ring);

  const int found = DumpSnapshot(snap.data(), snap.size(), opt, stdout);
  return found < 0 ? 4 : 0;
}
