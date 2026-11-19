// CRC32 (reflected, poly 0xEDB88320) over raw bytes. The table is built
// at compile time: no runtime initialization, safe to link into the hook
// DLL (spec 12.4 — no CRT-thread-safe static guards on the hook path).
#pragma once

#include <cstddef>
#include <cstdint>

uint32_t Crc32(const uint8_t* data, size_t size);
