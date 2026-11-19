// Unit tests for the tray payload decoder: the measured TRAYDATA layout
// (stage 0 spike S2, docs/spikes/02-s2-traydata-layout.md) applied to
// the record payload convention (dwData + cbData + data, plan task 3).
#include "TrayPayload.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

constexpr uint32_t kCopydataHeader = 8;  // dwData + cbData mini-header

void PutU32(std::vector<uint8_t>& p, size_t off, uint32_t v) {
  memcpy(p.data() + off, &v, sizeof(v));
}

void PutU16(std::vector<uint8_t>& p, size_t off, uint16_t v) {
  memcpy(p.data() + off, &v, sizeof(v));
}

// A well-formed tray message payload: dwData=1, cbData=1484, measured
// TRAYDATA layout inside.
std::vector<uint8_t> MakeTrayPayload(uint32_t nim, const wchar_t* tip) {
  std::vector<uint8_t> p(kCopydataHeader + kTrayPayloadSize, 0);
  PutU32(p, 0, 1);                       // dwData == 1 (tray)
  PutU32(p, 4, kTrayPayloadSize);        // cbData
  PutU32(p, kCopydataHeader + 0, kTrayMsgMagic);
  PutU32(p, kCopydataHeader + kTrayOffNim, nim);
  PutU32(p, kCopydataHeader + kTrayOffPackedNidSize, 956);
  PutU32(p, kCopydataHeader + kTrayOffHwnd, 0x70D9E);
  PutU32(p, kCopydataHeader + kTrayOffUid, 3);
  PutU32(p, kCopydataHeader + kTrayOffFlags, 0xA4);
  PutU32(p, kCopydataHeader + kTrayOffCallbackMsg, 0x13A0);
  PutU32(p, kCopydataHeader + kTrayOffIcon, 0x1002B);
  size_t tipOff = kCopydataHeader + kTrayOffTip;
  for (size_t i = 0; tip[i] != L'\0'; ++i) {
    PutU16(p, tipOff + i * 2, static_cast<uint16_t>(tip[i]));
  }
  return p;
}

TEST(TrayPayload, DecodesMeasuredLayout) {
  const std::vector<uint8_t> p =
      MakeTrayPayload(1, L"Physical memory: 57.543%");  // NIM_MODIFY
  TrayMessage msg = {};
  EXPECT_EQ(DecodeTrayPayload(p.data(), static_cast<uint32_t>(p.size()),
                              &msg),
            TrayDecodeStatus::Ok);
  EXPECT_EQ(msg.nim, 1u);
  EXPECT_EQ(msg.hwnd, 0x70D9Eu);
  EXPECT_EQ(msg.uid, 3u);
  EXPECT_EQ(msg.flags, 0xA4u);
  EXPECT_EQ(msg.callbackMsg, 0x13A0u);
  EXPECT_EQ(msg.icon, 0x1002Bu);
  EXPECT_FALSE(msg.truncated);
  EXPECT_STREQ(msg.tip, L"Physical memory: 57.543%");
}

TEST(TrayPayload, EscapesMultiLineCyrillicTip) {
  // The tip measured on the live tray (Mem Reduct): Cyrillic with
  // embedded CR LF, decoded from the S2 log.
  const std::vector<uint8_t> p =
      MakeTrayPayload(1, L"\x0424\x0438\x0437: 57%\r\n\x0424\x0430\x0439\x043b: 4%");
  TrayMessage msg = {};
  ASSERT_EQ(DecodeTrayPayload(p.data(), static_cast<uint32_t>(p.size()),
                              &msg),
            TrayDecodeStatus::Ok);
  EXPECT_STREQ(msg.tip, L"\x0424\x0438\x0437: 57%\\r\\n\x0424\x0430\x0439\x043b: 4%");
}

TEST(TrayPayload, ReplacesOtherControlChars) {
  // Note the split literals: a hex escape is greedy and would otherwise
  // swallow the following 'b' into \x0001b.
  std::vector<uint8_t> p = MakeTrayPayload(0, L"a" L"\x0001" L"b");
  TrayMessage msg = {};
  ASSERT_EQ(DecodeTrayPayload(p.data(), static_cast<uint32_t>(p.size()),
                              &msg),
            TrayDecodeStatus::Ok);
  EXPECT_STREQ(msg.tip, L"a.b");
}

TEST(TrayPayload, NonTraySubtypeIsNotTray) {
  std::vector<uint8_t> p = MakeTrayPayload(1, L"x");
  PutU32(p, 0, 3);  // dwData == 3: a non-tray WM_COPYDATA subtype
  TrayMessage msg = {};
  EXPECT_EQ(DecodeTrayPayload(p.data(), static_cast<uint32_t>(p.size()),
                              &msg),
            TrayDecodeStatus::NotTray);
}

TEST(TrayPayload, ShortPayloadIsInvalid) {
  // dwData == 1 claims a tray message, but the data is truncated: a
  // parser-level anomaly, not "ignore" (spec 7.3 counts parser errors).
  std::vector<uint8_t> p = MakeTrayPayload(1, L"x");
  p.resize(kCopydataHeader + 100);
  TrayMessage msg = {};
  EXPECT_EQ(DecodeTrayPayload(p.data(), static_cast<uint32_t>(p.size()),
                              &msg),
            TrayDecodeStatus::Invalid);
}

TEST(TrayPayload, WrongMagicIsInvalid) {
  std::vector<uint8_t> p = MakeTrayPayload(1, L"x");
  PutU32(p, kCopydataHeader, 0xDEADBEEF);
  TrayMessage msg = {};
  EXPECT_EQ(DecodeTrayPayload(p.data(), static_cast<uint32_t>(p.size()),
                              &msg),
            TrayDecodeStatus::Invalid);
}

TEST(TrayPayload, GarbageIsInvalid) {
  std::vector<uint8_t> p(kCopydataHeader + kTrayPayloadSize, 0xE7);
  PutU32(p, 0, 1);  // claims tray, rest is garbage
  TrayMessage msg = {};
  EXPECT_EQ(DecodeTrayPayload(p.data(), static_cast<uint32_t>(p.size()),
                              &msg),
            TrayDecodeStatus::Invalid);
}

TEST(TrayPayload, FlagsTruncationMismatch) {
  // cbData claims more than the payload actually carries.
  std::vector<uint8_t> p = MakeTrayPayload(1, L"x");
  PutU32(p, 4, kTrayPayloadSize + 500);
  TrayMessage msg = {};
  EXPECT_EQ(DecodeTrayPayload(p.data(), static_cast<uint32_t>(p.size()),
                              &msg),
            TrayDecodeStatus::Ok);
  EXPECT_TRUE(msg.truncated);
}

TEST(TrayPayload, TinyPayloadIsInvalid) {
  const uint8_t p[] = {1, 2, 3, 4, 5, 6, 7};
  TrayMessage msg = {};
  EXPECT_EQ(DecodeTrayPayload(p, sizeof(p), &msg),
            TrayDecodeStatus::Invalid);
}

}  // namespace
