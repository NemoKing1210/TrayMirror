// Unit tests for TrayModel: the spec 4.2 processing rules applied to
// decoded TrayMessages. No Explorer, no real processes: the icon
// operations and the owner identity resolvers are injectable.
#include "core/TrayModel.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>

namespace {

int g_copyCalls = 0;
int g_destroyCalls = 0;
bool g_copyFails = false;
int g_pidCalls = 0;
int g_exeCalls = 0;

HICON CopyCounting(HICON h) {
  ++g_copyCalls;
  if (g_copyFails) {
    return nullptr;
  }
  return CopyIcon(h);
}

BOOL DestroyCounting(HICON h) {
  ++g_destroyCalls;
  return DestroyIcon(h);
}

bool PidCounting(HWND /*hwnd*/, DWORD* pid, DWORD* tid) {
  ++g_pidCalls;
  *pid = 4242;
  *tid = 111;
  return true;
}

std::wstring ExeCounting(DWORD /*pid*/) {
  ++g_exeCalls;
  return L"C:\\apps\\test.exe";
}

HICON TestIcon() {
  return LoadIconW(nullptr, MAKEINTRESOURCEW(32512) /*IDI_APPLICATION*/);
}

TrayMessage MakeMsg(uint32_t nim, uint32_t hwnd, uint32_t uid, uint32_t flags) {
  TrayMessage m = {};
  m.nim = nim;
  m.hwnd = hwnd;
  m.uid = uid;
  m.flags = flags;
  m.callbackMsg = 0x13A0;
  m.icon = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(TestIcon()));
  wcscpy_s(m.tip, 130, L"tip-a");
  return m;
}

constexpr uint32_t kNifMessage = 0x1;
constexpr uint32_t kNifIcon = 0x2;
constexpr uint32_t kNifTip = 0x4;
constexpr uint32_t kNifState = 0x8;

class ModelTest : public ::testing::Test {
 protected:
  void SetUp() override {
    g_copyCalls = 0;
    g_destroyCalls = 0;
    g_copyFails = false;
    g_pidCalls = 0;
    g_exeCalls = 0;
    window_ = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x1234));
  }

  traymirror::TrayModel MakeModel() {
    traymirror::IconOps ops;
    ops.copy = CopyCounting;
    ops.destroy = DestroyCounting;
    return traymirror::TrayModel(ops, PidCounting, ExeCounting);
  }

  HWND window_ = nullptr;
};

TEST_F(ModelTest, AddCreatesIconWithOwnedCopy) {
  traymirror::TrayModel model = MakeModel();
  const HICON source = TestIcon();
  TrayMessage msg =
      MakeMsg(0, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(window_)), 1,
              kNifMessage | kNifIcon | kNifTip);
  msg.icon = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(source));

  ASSERT_TRUE(model.Apply(msg));
  ASSERT_EQ(model.icons().size(), 1u);
  const traymirror::TrayIcon& icon = model.icons()[0];
  EXPECT_NE(icon.iconCopy, nullptr);
  EXPECT_NE(icon.iconCopy, source);
  EXPECT_EQ(g_copyCalls, 1);
  EXPECT_STREQ(icon.tip.c_str(), L"tip-a");
  EXPECT_EQ(icon.callbackMessage, 0x13A0u);
}

TEST_F(ModelTest, AddIsIdempotentUpsert) {
  traymirror::TrayModel model = MakeModel();
  TrayMessage first = MakeMsg(
      0, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(window_)), 1,
      kNifTip);
  ASSERT_TRUE(model.Apply(first));
  TrayMessage second = first;
  wcscpy_s(second.tip, 130, L"tip-b");  // re-add after a resync (R4)
  EXPECT_TRUE(model.Apply(second));
  ASSERT_EQ(model.icons().size(), 1u);
  EXPECT_STREQ(model.icons()[0].tip.c_str(), L"tip-b");
}

TEST_F(ModelTest, ModifyAppliesOnlyFlaggedFields) {
  traymirror::TrayModel model = MakeModel();
  TrayMessage add = MakeMsg(
      0, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(window_)), 1,
      kNifMessage | kNifTip);
  ASSERT_TRUE(model.Apply(add));

  TrayMessage mod = add;
  mod.nim = 1;
  mod.flags = kNifTip;
  mod.callbackMsg = 0x9999;  // not flagged: must be ignored
  wcscpy_s(mod.tip, 130, L"tip-new");
  EXPECT_TRUE(model.Apply(mod));

  const traymirror::TrayIcon& icon = model.icons()[0];
  EXPECT_STREQ(icon.tip.c_str(), L"tip-new");
  EXPECT_EQ(icon.callbackMessage, 0x13A0u);  // untouched
}

TEST_F(ModelTest, ModifyStateHonorsMask) {
  traymirror::TrayModel model = MakeModel();
  TrayMessage add = MakeMsg(
      0, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(window_)), 1, 0);
  ASSERT_TRUE(model.Apply(add));

  TrayMessage mod = add;
  mod.nim = 1;
  mod.flags = kNifState;
  mod.state = 0x1;  // NIS_HIDDEN
  mod.stateMask = 0x1;
  ASSERT_TRUE(model.Apply(mod));
  EXPECT_EQ(model.icons()[0].state, 0x1u);

  mod.state = 0x2;  // NIS_SHAREDICON
  mod.stateMask = 0x2;  // hidden bit outside the mask: preserved
  ASSERT_TRUE(model.Apply(mod));
  EXPECT_EQ(model.icons()[0].state, 0x3u);
  EXPECT_EQ(model.icons()[0].stateMask, 0x2u);
}

TEST_F(ModelTest, DeleteRemovesAndDestroysCopy) {
  traymirror::TrayModel model = MakeModel();
  TrayMessage add = MakeMsg(
      0, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(window_)), 1,
      kNifIcon);
  ASSERT_TRUE(model.Apply(add));
  ASSERT_EQ(g_destroyCalls, 0);

  TrayMessage del = add;
  del.nim = 2;
  EXPECT_TRUE(model.Apply(del));
  EXPECT_TRUE(model.icons().empty());
  EXPECT_EQ(g_destroyCalls, 1);
}

TEST_F(ModelTest, SetVersionStoresVersion) {
  traymirror::TrayModel model = MakeModel();
  TrayMessage add = MakeMsg(
      0, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(window_)), 1, 0);
  ASSERT_TRUE(model.Apply(add));
  ASSERT_EQ(model.icons()[0].version, 0u);

  TrayMessage ver = add;
  ver.nim = 4;
  ver.flags = 0;
  ver.version = 4;
  EXPECT_TRUE(model.Apply(ver));
  EXPECT_EQ(model.icons()[0].version, 4u);

  EXPECT_FALSE(model.Apply(ver));  // same version: no change
}

TEST_F(ModelTest, SetFocusIsNoOp) {
  traymirror::TrayModel model = MakeModel();
  TrayMessage add = MakeMsg(
      0, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(window_)), 1, 0);
  ASSERT_TRUE(model.Apply(add));
  TrayMessage focus = add;
  focus.nim = 3;
  EXPECT_FALSE(model.Apply(focus));
  EXPECT_EQ(model.icons().size(), 1u);
}

TEST_F(ModelTest, GuidKeyUsedWhenNifGuidSet) {
  traymirror::TrayModel model = MakeModel();
  TrayMessage a =
      MakeMsg(0, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(window_)), 0,
              kNifTip | kNifGuid);
  a.hasGuid = true;
  a.guid = GUID{0x11111111, 0, 0, {1}};
  ASSERT_TRUE(model.Apply(a));

  TrayMessage b = a;
  b.guid = GUID{0x22222222, 0, 0, {2}};  // same hwnd/uid, different GUID
  ASSERT_TRUE(model.Apply(b));

  EXPECT_EQ(model.icons().size(), 2u);
  EXPECT_EQ(model.icons()[0].key.kind, traymirror::IconKey::Kind::kGuid);
}

TEST_F(ModelTest, Wow64HandleSignExtended) {
  traymirror::TrayModel model = MakeModel();
  TrayMessage msg = MakeMsg(0, 0x80000001u, 1, 0);  // WOW64: high bit set
  ASSERT_TRUE(model.Apply(msg));
  ASSERT_EQ(model.icons().size(), 1u);
  const HWND expected =
      reinterpret_cast<HWND>(static_cast<INT_PTR>(static_cast<int32_t>(
          static_cast<uint32_t>(0x80000001u))));
  EXPECT_EQ(model.icons()[0].key.hwnd, expected);
  EXPECT_NE(model.icons()[0].key.hwnd,
            reinterpret_cast<HWND>(static_cast<uintptr_t>(0x80000001u)));
}

TEST_F(ModelTest, CopyFailureDegradesToPlaceholder) {
  g_copyFails = true;
  traymirror::TrayModel model = MakeModel();
  TrayMessage msg = MakeMsg(
      0, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(window_)), 1,
      kNifIcon);
  EXPECT_TRUE(model.Apply(msg));
  ASSERT_EQ(model.icons().size(), 1u);
  EXPECT_EQ(model.icons()[0].iconCopy, nullptr);
}

TEST_F(ModelTest, OwnerIdentityResolvedOnce) {
  traymirror::TrayModel model = MakeModel();
  TrayMessage a = MakeMsg(
      0, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(window_)), 1, 0);
  TrayMessage b = MakeMsg(
      0, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(window_)), 2, 0);
  ASSERT_TRUE(model.Apply(a));
  ASSERT_TRUE(model.Apply(b));
  EXPECT_EQ(g_pidCalls, 1);
  EXPECT_EQ(g_exeCalls, 1);
  EXPECT_STREQ(model.icons()[0].exePath.c_str(), L"C:\\apps\\test.exe");
}

TEST_F(ModelTest, SerialBumpsOnlyOnIconReplace) {
  traymirror::TrayModel model = MakeModel();
  TrayMessage add = MakeMsg(
      0, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(window_)), 1,
      kNifIcon | kNifTip);
  ASSERT_TRUE(model.Apply(add));
  const UINT serialAfterAdd = model.icons()[0].iconSerial;
  EXPECT_GT(serialAfterAdd, 0u);

  TrayMessage tipMod = add;
  tipMod.nim = 1;
  tipMod.flags = kNifTip;
  wcscpy_s(tipMod.tip, 130, L"tip-moved");
  ASSERT_TRUE(model.Apply(tipMod));
  EXPECT_EQ(model.icons()[0].iconSerial, serialAfterAdd);

  TrayMessage iconMod = add;
  iconMod.nim = 1;
  iconMod.flags = kNifIcon;
  ASSERT_TRUE(model.Apply(iconMod));
  EXPECT_GT(model.icons()[0].iconSerial, serialAfterAdd);
}

}  // namespace

// Stage 3 (spec 4.1/4.2): a GUID-keyed icon still registers a uID, and
// that uID is what the owner's callback must carry — stage 2 dispatched
// GUID icons with uid 0 (ADR-0005 debt).
TEST_F(ModelTest, GuidIconKeepsItsUidForDispatch) {
  traymirror::TrayModel model = MakeModel();
  TrayMessage msg =
      MakeMsg(0, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(window_)), 7,
              kNifTip | kNifGuid);
  msg.hasGuid = true;
  msg.guid = GUID{0x33333333, 0, 0, {3}};
  ASSERT_TRUE(model.Apply(msg));

  ASSERT_EQ(model.icons().size(), 1u);
  EXPECT_EQ(model.icons()[0].key.kind, traymirror::IconKey::Kind::kGuid);
  EXPECT_EQ(model.icons()[0].key.uid, 7u);
}

// Stage 3 (spec 4.5): promotion state decides strip versus overflow and
// is applied from outside the message stream.
TEST_F(ModelTest, PromotionChangeReportsAVisibleChange) {
  traymirror::TrayModel model = MakeModel();
  TrayMessage msg =
      MakeMsg(0, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(window_)), 1,
              kNifTip);
  ASSERT_TRUE(model.Apply(msg));
  ASSERT_EQ(model.icons().size(), 1u);
  EXPECT_TRUE(model.icons()[0].promoted);  // the default: shown

  const traymirror::IconKey key = model.icons()[0].key;
  EXPECT_TRUE(model.SetPromotion(key, false));
  EXPECT_FALSE(model.icons()[0].promoted);
  EXPECT_FALSE(model.SetPromotion(key, false));  // nothing changed
  EXPECT_TRUE(model.SetPromotion(key, true));
  EXPECT_TRUE(model.icons()[0].promoted);
  EXPECT_FALSE(model.SetPromotion(traymirror::IconKey{}, true));  // unknown key
}

// Spec 4.1 lists state/stateMask; NIS_SHAREDICON must be stored like any
// other state bit without making the icon hidden.
TEST_F(ModelTest, SharedIconStateIsStored) {
  traymirror::TrayModel model = MakeModel();
  TrayMessage add =
      MakeMsg(0, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(window_)), 1,
              kNifTip);
  ASSERT_TRUE(model.Apply(add));

  constexpr uint32_t kNisSharedIcon = 0x2;
  TrayMessage modify = add;
  modify.nim = 1;  // NIM_MODIFY
  modify.flags = kNifState;
  modify.state = kNisSharedIcon;
  modify.stateMask = kNisSharedIcon;
  ASSERT_TRUE(model.Apply(modify));

  const traymirror::TrayIcon& icon = model.icons()[0];
  EXPECT_EQ(icon.state & kNisSharedIcon, kNisSharedIcon);
  EXPECT_FALSE(icon.hidden());
}

// Stage 3 uses lastSeen for promotion matching and diagnostics: every
// applied message refreshes it.
TEST_F(ModelTest, LastSeenAdvancesOnEveryApply) {
  traymirror::TrayModel model = MakeModel();
  TrayMessage msg =
      MakeMsg(0, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(window_)), 1,
              kNifTip);
  ASSERT_TRUE(model.Apply(msg));
  const ULONGLONG first = model.icons()[0].lastSeen;
  EXPECT_NE(first, 0u);

  Sleep(5);
  TrayMessage modify = msg;
  modify.nim = 1;
  modify.flags = kNifTip;
  wcscpy_s(modify.tip, 130, L"tip-b");
  model.Apply(modify);
  EXPECT_GE(model.icons()[0].lastSeen, first);
  EXPECT_STREQ(model.icons()[0].tip.c_str(), L"tip-b");
}

// A tray icon whose owner window is gone can never be clicked again
// (spec 4.4): removal drops the model entry and the icon copy with it.
TEST(ModelRemoval, ReleasesTheIconCopy) {
  g_copyCalls = 0;
  g_destroyCalls = 0;
  traymirror::TrayModel model(
      traymirror::IconOps{.copy = CopyCounting, .destroy = DestroyCounting},
      &PidCounting, &ExeCounting);
  constexpr uint32_t kNimAdd = 0;
  ASSERT_TRUE(model.Apply(MakeMsg(kNimAdd, 0x1111, 1, kNifIcon)));
  ASSERT_EQ(g_destroyCalls, 0);

  EXPECT_TRUE(model.RemoveOwner(traymirror::SignExtendHwnd(0x1111)));
  EXPECT_TRUE(model.icons().empty());
  EXPECT_EQ(g_destroyCalls, 1);  // the copy, released with the entry
  EXPECT_FALSE(model.RemoveOwner(traymirror::SignExtendHwnd(0x1111)));  // already gone
  EXPECT_EQ(g_destroyCalls, 1);
}

TEST(ModelRemoval, KeepsOtherOwners) {
  g_copyCalls = 0;
  g_destroyCalls = 0;
  traymirror::TrayModel model(
      traymirror::IconOps{.copy = CopyCounting, .destroy = DestroyCounting},
      &PidCounting, &ExeCounting);
  constexpr uint32_t kNimAdd = 0;
  ASSERT_TRUE(model.Apply(MakeMsg(kNimAdd, 0x1111, 1, kNifIcon)));
  ASSERT_TRUE(model.Apply(MakeMsg(kNimAdd, 0x2222, 2, kNifIcon)));
  ASSERT_EQ(model.icons().size(), 2U);

  EXPECT_TRUE(model.RemoveOwner(traymirror::SignExtendHwnd(0x1111)));
  ASSERT_EQ(model.icons().size(), 1U);
  EXPECT_EQ(model.icons()[0].key.uid, 2U);
  EXPECT_EQ(g_destroyCalls, 1);

  EXPECT_FALSE(model.RemoveOwner(nullptr));  // no owner, nothing to do
  EXPECT_EQ(model.icons().size(), 1U);
}
