#pragma once

#include <cstddef>
#include <cstdint>

// TRAYDATA payload layout for WM_COPYDATA (dwData == 1) arriving at
// Shell_TrayWnd on Windows 11 build 26300.
//
// MEASURED, not invented (spec 12.2): derived from emulator anchor values in
// spike S2 (docs/spikes/02-s2-traydata-layout.md, local docs). Verified for
// both x64 and WOW64 (x86) senders: the layout is bitness-independent — the
// system stores handles as u32 in the inter-process payload.
// Widen handles (sign-extend) only at use time, not at parse time.
inline constexpr bool kLayoutKnown = true;

inline constexpr uint32_t kTrayMsgMagic = 0x34753423;  // payload[0], all subtypes
inline constexpr size_t kTrayPayloadSize = 1484;       // fixed for every sender

inline constexpr size_t kOffNim = 4;          // uint32: 0 ADD, 1 MODIFY, 2 DELETE, 3 SETFOCUS, 4 SETVERSION
inline constexpr size_t kOffPackedNidSize = 8;  // uint32: 956, constant
inline constexpr size_t kOffHwnd = 12;        // uint32 (owner window handle)
inline constexpr size_t kOffUid = 16;          // uint32
inline constexpr size_t kOffFlags = 20;       // uint32: NIF_*
inline constexpr size_t kOffCallbackMsg = 24; // uint32
inline constexpr size_t kOffIcon = 28;        // uint32 (icon handle)
inline constexpr size_t kOffTip = 32;          // UTF-16, 128 wchars

inline constexpr uint32_t kTrayPackedNidSize = 956;  // observed value at kOffPackedNidSize
