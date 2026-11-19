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
