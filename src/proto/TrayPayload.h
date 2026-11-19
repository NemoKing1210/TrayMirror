// Tray message decoder: the MEASURED inter-process TRAYDATA layout from
// stage 0 spike S2 (docs/spikes/02-s2-traydata-layout.md, local docs;
// Windows 11 build 26300, x64 and WOW64 senders identical). Nothing here
// is invented: every offset was observed on the live tray (spec 12.2).
// The input is the record payload convention of plan task 3: the first
// 8 bytes are COPYDATASTRUCT.dwData and cbData, followed by cbData bytes
// of raw copy data.
#pragma once

#include <cstdint>

// Measured constants (spike S2).
inline constexpr uint32_t kTrayMsgMagic = 0x34753423;  // payload[0]
inline constexpr uint32_t kTrayPayloadSize = 1484;     // fixed for every sender
inline constexpr size_t kTrayOffNim = 4;               // 0 ADD, 1 MODIFY, 2 DELETE, 3 SETFOCUS, 4 SETVERSION
inline constexpr size_t kTrayOffPackedNidSize = 8;     // constant 956 on this build
inline constexpr size_t kTrayOffHwnd = 12;             // u32 (owner window handle)
inline constexpr size_t kTrayOffUid = 16;
inline constexpr size_t kTrayOffFlags = 20;             // NIF_*
inline constexpr size_t kTrayOffCallbackMsg = 24;
inline constexpr size_t kTrayOffIcon = 28;             // u32 (icon handle)
inline constexpr size_t kTrayOffTip = 32;              // UTF-16, 128 wchars

struct TrayMessage {
  uint32_t nim = 0;
  uint32_t hwnd = 0;
  uint32_t uid = 0;
  uint32_t flags = 0;
  uint32_t callbackMsg = 0;
  uint32_t icon = 0;
  wchar_t tip[130] = {};  // \r and \n escaped, other control chars '.'
  bool truncated = false;  // cbData claims more than the payload carries
};

enum class TrayDecodeStatus : uint8_t {
  NotTray,  // dwData != 1: not a tray WM_COPYDATA — ignore (spec 3.3)
  Ok,
  Invalid,  // dwData == 1 but the layout does not match the measured one:
            // a parser-level anomaly (spec 7.3 territory), never guessed
};

// Decodes a record payload. Never reads past payloadSize. Returns
// NotTray without touching out.
TrayDecodeStatus DecodeTrayPayload(const uint8_t* payload, uint32_t payloadSize,
                                   TrayMessage* out);
