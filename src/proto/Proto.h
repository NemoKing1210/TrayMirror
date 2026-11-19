// Stage 1 protocol constants (spec section 3). All IPC objects live in
// the session namespace (Local\) and are versioned by kProtoVersion.
#pragma once

#include <cstddef>
#include <cstdint>

inline constexpr uint32_t kProtoVersion = 1;
inline constexpr uint32_t kRingMagic = 0x47524D54;  // bytes 'TMRG', LE
inline constexpr uint32_t kRingSizeBytes = 1U << 20;  // spec 3.1: ~1 MB
inline constexpr uint32_t kMaxPayloadBytes = 4096;   // per record payload cap
inline constexpr size_t kRecordHeaderSize = 24;      // RingRecordHeader
inline constexpr size_t kRecordFooterSize = 4;       // trailing CRC32

// RingHeader.flags bit: the host raised diagnostic mode; the hook then
// also records non-tray WM_COPYDATA subtypes (spec 3.3).
inline constexpr uint32_t kHostFlagDiag = 0x1;

inline constexpr wchar_t kRingNameFmt[] = L"Local\\TrayMirror.Ring.v%u";
inline constexpr wchar_t kSignalEventName[] = L"Local\\TrayMirror.Signal";  // auto-reset
inline constexpr wchar_t kOffEventName[] = L"Local\\TrayMirror.Off";        // manual-reset, latched
inline constexpr wchar_t kSingleInstanceMutexName[] = L"Local\\TrayMirror.Single";
