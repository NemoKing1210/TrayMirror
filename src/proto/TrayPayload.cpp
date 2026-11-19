#include "TrayPayload.h"

#include <cstring>

namespace {

uint32_t RdU32(const uint8_t* p) {
  uint32_t v = 0;
  memcpy(&v, p, sizeof(v));
  return v;
}

constexpr uint32_t kCopydataHeader = 8;  // dwData + cbData

}  // namespace

TrayDecodeStatus DecodeTrayPayload(const uint8_t* payload,
                                   uint32_t payloadSize, TrayMessage* out) {
  if (out == nullptr || payload == nullptr || payloadSize < kCopydataHeader) {
    return TrayDecodeStatus::Invalid;
  }
  if (RdU32(payload) != 1) {
    return TrayDecodeStatus::NotTray;
  }

  const uint8_t* data = payload + kCopydataHeader;
  const uint32_t dataSize = payloadSize - kCopydataHeader;
  if (dataSize < kTrayPayloadSize) {
    return TrayDecodeStatus::Invalid;
  }
  if (RdU32(data) != kTrayMsgMagic) {
    return TrayDecodeStatus::Invalid;
  }

  out->nim = RdU32(data + kTrayOffNim);
  out->hwnd = RdU32(data + kTrayOffHwnd);
  out->uid = RdU32(data + kTrayOffUid);
  out->flags = RdU32(data + kTrayOffFlags);
  out->callbackMsg = RdU32(data + kTrayOffCallbackMsg);
  out->icon = RdU32(data + kTrayOffIcon);
  out->truncated = RdU32(payload + 4) > dataSize;

  // Tip: up to 64 wchars inside the measured payload, NUL-terminated; the
  // tail after the NUL is stale buffer bytes. \r and \n are escaped so a
  // record always renders as one line; other control chars become '.'.
  size_t o = 0;
  for (size_t i = 0; i < 64 && o + 2 < 130; ++i) {
    const size_t off = kTrayOffTip + (i * 2);
    if (off + 1 >= kTrayPayloadSize) {
      break;
    }
    uint16_t ch = 0;
    memcpy(&ch, data + off, sizeof(ch));
    if (ch == 0) {
      break;
    }
    if (ch == L'\r') {
      out->tip[o++] = L'\\';
      out->tip[o++] = L'r';
    } else if (ch == L'\n') {
      out->tip[o++] = L'\\';
      out->tip[o++] = L'n';
    } else if (ch < 0x20) {
      out->tip[o++] = L'.';
    } else {
      out->tip[o++] = static_cast<wchar_t>(ch);
    }
  }
  out->tip[o] = L'\0';
  return TrayDecodeStatus::Ok;
}
