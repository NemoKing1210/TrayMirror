#include "Crc32.h"

namespace {

constexpr uint32_t kCrcPoly = 0xEDB88320;

struct CrcTable {
  uint32_t v[256];
};

consteval CrcTable MakeCrcTable() {
  CrcTable t = {};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int bit = 0; bit < 8; ++bit) {
      c = (c & 1U) != 0 ? (kCrcPoly ^ (c >> 1)) : (c >> 1);
    }
    t.v[i] = c;
  }
  return t;
}

constexpr CrcTable kCrcTable = MakeCrcTable();

}  // namespace

uint32_t Crc32(const uint8_t* data, size_t size) {
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < size; ++i) {
    crc = kCrcTable.v[(crc ^ data[i]) & 0xFFU] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFF;
}
