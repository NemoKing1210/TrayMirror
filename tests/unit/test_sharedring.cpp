// Unit tests for the host-side shared ring creation: named mapping with
// a user+SYSTEM DACL, auto-reset signal event, read-only reopen (plan
// task 5).
#include "Proto.h"
#include "Ring.h"
#include "ipc/SharedRing.h"

#include <gtest/gtest.h>

#include <windows.h>

#include <aclapi.h>
#include <sddl.h>

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace {

std::wstring CurrentUserSid() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    return {};
  }
  DWORD size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<uint8_t> buf(size);
  wchar_t* sid = nullptr;
  if (GetTokenInformation(token, TokenUser, buf.data(), size, &size)) {
    const auto* user = reinterpret_cast<const TOKEN_USER*>(buf.data());
    ConvertSidToStringSidW(user->User.Sid, &sid);
  }
  CloseHandle(token);
  std::wstring out = sid != nullptr ? sid : L"";
  if (sid != nullptr) {
    LocalFree(sid);
  }
  return out;
}

std::set<std::wstring> MappingDaclSids(HANDLE map) {
  std::set<std::wstring> sids;
  PACL dacl = nullptr;
  PSECURITY_DESCRIPTOR sd = nullptr;
  if (GetSecurityInfo(map, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION,
                      nullptr, nullptr, &dacl, nullptr,
                      &sd) != ERROR_SUCCESS) {
    return sids;
  }
  for (DWORD i = 0; dacl != nullptr && i < dacl->AceCount; ++i) {
    void* ace = nullptr;
    if (!GetAce(dacl, i, &ace)) {
      continue;
    }
    const auto* header = static_cast<const ACE_HEADER*>(ace);
    if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
      continue;  // only allow ACEs are acceptable
    }
    const auto* allowed = static_cast<const ACCESS_ALLOWED_ACE*>(ace);
    wchar_t* sid = nullptr;
    ConvertSidToStringSidW(
        reinterpret_cast<PSID>(
            const_cast<DWORD*>(&allowed->SidStart)),
        &sid);
    if (sid != nullptr) {
      sids.insert(sid);
      LocalFree(sid);
    }
  }
  LocalFree(sd);
  return sids;
}

class SharedRingFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    err_[0] = L'\0';
    const bool ok = SharedRingCreate(&h_, 4321, err_, 256);
    ASSERT_TRUE(ok) << "create failed: " << err_;
  }
  void TearDown() override { SharedRingClose(&h_); }

  SharedRingHandle h_ = {};
  wchar_t err_[256] = {};
};

TEST_F(SharedRingFixture, HeaderIsPublished) {
  const volatile RingHeader* header = h_.header;
  EXPECT_EQ(header->magic, kRingMagic);
  EXPECT_EQ(header->protoVersion, kProtoVersion);
  EXPECT_EQ(header->headerSize, sizeof(RingHeader));
  EXPECT_EQ(header->ringSize, kRingSizeBytes);
  EXPECT_EQ(header->explorerPid, 4321u);
  EXPECT_EQ(header->writePos, 0u);
  EXPECT_EQ(header->readPos, 0u);
  EXPECT_EQ(header->flags, 0u);
  // The ring area starts right after the header.
  EXPECT_EQ(reinterpret_cast<uintptr_t>(h_.ring) -
                reinterpret_cast<uintptr_t>(h_.header),
            sizeof(RingHeader));
}

TEST_F(SharedRingFixture, DaclAllowsOnlySystemAndCurrentUser) {
  const std::set<std::wstring> sids = MappingDaclSids(h_.map);
  EXPECT_EQ(sids.size(), 2u);
  EXPECT_NE(sids.find(L"S-1-5-18"), sids.end());  // LOCAL SYSTEM
  EXPECT_NE(sids.find(CurrentUserSid()), sids.end());
  EXPECT_EQ(sids.find(L"S-1-1-0"), sids.end());  // no Everyone
}

TEST_F(SharedRingFixture, SecondCreateFails) {
  SharedRingHandle second = {};
  wchar_t err[256] = {};
  EXPECT_FALSE(SharedRingCreate(&second, 1, err, 256));
  EXPECT_NE(err[0], L'\0');
}

TEST_F(SharedRingFixture, SignalEventIsAutoResetAndNotSignaled) {
  EXPECT_EQ(WaitForSingleObject(h_.signalEvent, 0), WAIT_TIMEOUT);
  EXPECT_TRUE(SetEvent(h_.signalEvent));
  EXPECT_EQ(WaitForSingleObject(h_.signalEvent, 0), WAIT_OBJECT_0);
  // Auto-reset: consumed by the first wait.
  EXPECT_EQ(WaitForSingleObject(h_.signalEvent, 0), WAIT_TIMEOUT);
}

TEST(SharedRingOpen, ReadOnlyReopenSeesSameHeader) {
  SharedRingHandle creator = {};
  wchar_t err[256] = {};
  ASSERT_TRUE(SharedRingCreate(&creator, 777, err, 256));

  SharedRingHandle reader = {};
  ASSERT_TRUE(SharedRingOpenRead(&reader));
  EXPECT_EQ(reader.header->magic, kRingMagic);
  EXPECT_EQ(reader.header->explorerPid, 777u);
  EXPECT_EQ(reader.header->ringSize, kRingSizeBytes);

  // Writer-side view and read-only view alias the same bytes.
  creator.header->writePos = 12345;
  EXPECT_EQ(reader.header->writePos, 12345u);

  SharedRingClose(&reader);
  SharedRingClose(&creator);
}

TEST(SharedRingOpen, FailsWhenNoMappingExists) {
  SharedRingHandle creator = {};
  wchar_t err[256] = {};
  ASSERT_TRUE(SharedRingCreate(&creator, 1, err, 256));
  SharedRingClose(&creator);

  SharedRingHandle reader = {};
  EXPECT_FALSE(SharedRingOpenRead(&reader));
}

}  // namespace
