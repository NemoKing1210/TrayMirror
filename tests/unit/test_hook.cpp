// Unit tests for the production hook DLL: exercised in THIS process by
// calling TrayHookProc directly against a locally created shared ring —
// no Explorer contact (plan task 7).
#include "Proto.h"
#include "Ring.h"
#include "RingReader.h"
#include "TrayPayload.h"
#include "ipc/SharedRing.h"

#include <windows.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

constexpr wchar_t kDllName[] = L"TrayMirrorHook.dll";

struct HookApi {
  HMODULE dll = nullptr;
  LRESULT(CALLBACK* proc)(int, WPARAM, LPARAM) = nullptr;
};

HookApi LoadHook() {
  HookApi api;
  api.dll = LoadLibraryW(TRAYMIRROR_HOOK_DLL_PATH);
  if (api.dll == nullptr) {
    return api;
  }
  api.proc = reinterpret_cast<decltype(api.proc)>(
      GetProcAddress(api.dll, "TrayHookProc"));
  return api;
}

void PutU32(std::vector<uint8_t>& p, size_t off, uint32_t v) {
  memcpy(p.data() + off, &v, sizeof(v));
}

// Raw TRAYDATA bytes per the measured layout (dwData/cbData live in the
// COPYDATASTRUCT, not here).
std::vector<uint8_t> MakeTrayData() {
  std::vector<uint8_t> d(kTrayPayloadSize, 0);
  PutU32(d, 0, kTrayMsgMagic);
  PutU32(d, kTrayOffNim, 1);  // NIM_MODIFY
  PutU32(d, kTrayOffPackedNidSize, 956);
  PutU32(d, kTrayOffHwnd, 0x70D9E);
  PutU32(d, kTrayOffUid, 7);
  PutU32(d, kTrayOffFlags, 0xA4);
  PutU32(d, kTrayOffCallbackMsg, 0x13A0);
  PutU32(d, kTrayOffIcon, 0x1002B);
  const wchar_t kTip[] = L"hook-test-tip";
  memcpy(d.data() + kTrayOffTip, kTip, sizeof(kTip));
  return d;
}

struct CallContext {
  COPYDATASTRUCT cds = {};
  CWPRETSTRUCT cwp = {};
};

void BuildCall(CallContext* ctx, DWORD dwData, const void* data,
               DWORD cbData) {
  ctx->cds.dwData = dwData;
  ctx->cds.cbData = cbData;
  ctx->cds.lpData = const_cast<void*>(data);
  ctx->cwp.message = WM_COPYDATA;
  ctx->cwp.hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x1234));
  ctx->cwp.lParam = reinterpret_cast<LPARAM>(&ctx->cds);
}

class HookFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    err_[0] = L'\0';
    // The mapping may be an adopted orphan (the hook DLL keeps a view
    // alive between tests): reset the logical state either way.
    ASSERT_TRUE(SharedRingCreate(&ring_, 999, err_, 256)) << err_;
    ring_.header->writePos = 0;
    ring_.header->readPos = 0;
    ring_.header->hookAlive = 0;
    ring_.header->droppedCount = 0;
    ring_.header->flags = 0;
    // The kill switch event, unsignaled: normally armed by the host.
    off_ = CreateEventW(nullptr, TRUE, FALSE, kOffEventName);
    ASSERT_NE(off_, nullptr);
    ResetEvent(off_);

    api_ = LoadHook();
    ASSERT_NE(api_.dll, nullptr) << "LoadLibrary failed: " << GetLastError();
    ASSERT_NE(api_.proc, nullptr) << "TrayHookProc export missing";
  }
  void TearDown() override {
    SharedRingClose(&ring_);
    if (off_ != nullptr) {
      CloseHandle(off_);
    }
    if (api_.dll != nullptr) {
      FreeLibrary(api_.dll);
    }
  }

  RingReadStatus ReadNext(RingRecordHeader* hdr, const uint8_t** payload) {
    uint8_t buf[kRecordHeaderSize + kMaxPayloadBytes + kRecordFooterSize];
    return RingReaderGet(ring_.header, ring_.ring, buf, sizeof(buf), hdr,
                         payload);
  }

  SharedRingHandle ring_ = {};
  HANDLE off_ = nullptr;
  HookApi api_ = {};
  wchar_t err_[256] = {};
};

TEST_F(HookFixture, TrayCopydataLandsInRingDecodable) {
  const std::vector<uint8_t> data = MakeTrayData();
  CallContext ctx;
  BuildCall(&ctx, 1, data.data(), kTrayPayloadSize);

  const uint32_t aliveBefore = ring_.header->hookAlive;
  api_.proc(/*code=*/0, /*wp=*/0, reinterpret_cast<LPARAM>(&ctx.cwp));

  RingRecordHeader hdr = {};
  const uint8_t* payload = nullptr;
  ASSERT_EQ(ReadNext(&hdr, &payload), RingReadStatus::Ok);
  EXPECT_EQ(hdr.type, kRecNotifyCopydata);

  TrayMessage msg = {};
  EXPECT_EQ(DecodeTrayPayload(payload, hdr.payloadSize, &msg),
            TrayDecodeStatus::Ok);
  EXPECT_EQ(msg.nim, 1u);
  EXPECT_EQ(msg.hwnd, 0x70D9Eu);
  EXPECT_EQ(msg.uid, 7u);
  EXPECT_STREQ(msg.tip, L"hook-test-tip");
  EXPECT_GT(ring_.header->hookAlive, aliveBefore);
  EXPECT_EQ(ring_.header->droppedCount, 0u);
}

TEST_F(HookFixture, NonTraySubtypeFilteredUnlessDiag) {
  const std::vector<uint8_t> data = MakeTrayData();

  // dwData == 2, diagnostic off: filtered.
  CallContext ctx;
  BuildCall(&ctx, 2, data.data(), kTrayPayloadSize);
  api_.proc(0, 0, reinterpret_cast<LPARAM>(&ctx.cwp));
  EXPECT_EQ(ring_.header->writePos, ring_.header->readPos);

  // Diagnostic on: recorded as kRecOtherCopydata.
  ring_.header->flags = kHostFlagDiag;
  api_.proc(0, 0, reinterpret_cast<LPARAM>(&ctx.cwp));
  ring_.header->flags = 0;

  RingRecordHeader hdr = {};
  const uint8_t* payload = nullptr;
  ASSERT_EQ(ReadNext(&hdr, &payload), RingReadStatus::Ok);
  EXPECT_EQ(hdr.type, kRecOtherCopydata);
  TrayMessage msg = {};
  EXPECT_EQ(DecodeTrayPayload(payload, hdr.payloadSize, &msg),
            TrayDecodeStatus::NotTray);
}

TEST_F(HookFixture, OversizeCbDataTruncatedToPayloadCap) {
  const std::vector<uint8_t> data(kMaxPayloadBytes + 2048, 0x5A);
  CallContext ctx;
  BuildCall(&ctx, 1, data.data(), kMaxPayloadBytes + 2048);
  api_.proc(0, 0, reinterpret_cast<LPARAM>(&ctx.cwp));

  RingRecordHeader hdr = {};
  const uint8_t* payload = nullptr;
  ASSERT_EQ(ReadNext(&hdr, &payload), RingReadStatus::Ok);
  EXPECT_EQ(hdr.payloadSize, kMaxPayloadBytes);  // 8 header + 4088 data
}

TEST_F(HookFixture, ZeroCbDataRecordsMiniHeaderOnly) {
  CallContext ctx;
  BuildCall(&ctx, 1, nullptr, 0);
  api_.proc(0, 0, reinterpret_cast<LPARAM>(&ctx.cwp));

  RingRecordHeader hdr = {};
  const uint8_t* payload = nullptr;
  ASSERT_EQ(ReadNext(&hdr, &payload), RingReadStatus::Ok);
  EXPECT_EQ(hdr.payloadSize, 8u);
}

TEST_F(HookFixture, PoisonedLpDataRecordsHeaderOnlyProcessAlive) {
  SYSTEM_INFO si = {};
  GetSystemInfo(&si);
  void* page = VirtualAlloc(nullptr, si.dwPageSize, MEM_COMMIT,
                             PAGE_NOACCESS);
  ASSERT_NE(page, nullptr);
  CallContext ctx;
  BuildCall(&ctx, 1, page, kTrayPayloadSize);
  api_.proc(0, 0, reinterpret_cast<LPARAM>(&ctx.cwp));
  VirtualFree(page, 0, MEM_RELEASE);

  // The process survived the SEH boundary; the record carries the
  // mini-header with the real cbData but no data bytes.
  RingRecordHeader hdr = {};
  const uint8_t* payload = nullptr;
  ASSERT_EQ(ReadNext(&hdr, &payload), RingReadStatus::Ok);
  TrayMessage msg = {};
  EXPECT_EQ(DecodeTrayPayload(payload, hdr.payloadSize, &msg),
            TrayDecodeStatus::Invalid);
}

TEST_F(HookFixture, ForeignVersionHeaderMeansNoOp) {
  ring_.header->protoVersion = kProtoVersion + 1;
  const std::vector<uint8_t> data = MakeTrayData();
  CallContext ctx;
  BuildCall(&ctx, 1, data.data(), kTrayPayloadSize);
  const uint64_t posBefore = ring_.header->writePos;
  api_.proc(0, 0, reinterpret_cast<LPARAM>(&ctx.cwp));
  EXPECT_EQ(ring_.header->writePos, posBefore);
  EXPECT_EQ(ring_.header->droppedCount, 0u);

  // Restore: the adopted-ring SetUp of later tests validates this header.
  ring_.header->protoVersion = kProtoVersion;
}

TEST_F(HookFixture, NoMappingMeansSilentNoOp) {
  // Destroy the mapping first: the hook's lazy open must fail silently.
  SharedRingClose(&ring_);
  ring_ = {};

  const std::vector<uint8_t> data = MakeTrayData();
  CallContext ctx;
  BuildCall(&ctx, 1, data.data(), kTrayPayloadSize);
  api_.proc(0, 0, reinterpret_cast<LPARAM>(&ctx.cwp));  // must not crash

  // Recreate for TearDown bookkeeping.
  wchar_t err[256] = {};
  ASSERT_TRUE(SharedRingCreate(&ring_, 999, err, 256)) << err;
}

TEST(HookKillSwitch, EventIsHeldForTheDllLifetime) {
  // A --disable-hook arm must stay observable from the moment the hook
  // DLL exists anywhere — even before the first tray message reaches
  // it (the lazy per-message open would leave exactly that window: an
  // orphaned hook with no messages yet, and an arm that evaporates
  // with the arming process). While the DLL is loaded, the named Off
  // event must exist with NO other handle holding it.
  HookApi api = LoadHook();
  ASSERT_NE(api.dll, nullptr) << "LoadLibrary failed: " << GetLastError();

  HANDLE off = OpenEventW(EVENT_MODIFY_STATE, FALSE, kOffEventName);
  EXPECT_NE(off, nullptr) << "the loaded hook must hold the Off event";
  if (off != nullptr) {
    CloseHandle(off);
  }
  FreeLibrary(api.dll);
}

}  // namespace
