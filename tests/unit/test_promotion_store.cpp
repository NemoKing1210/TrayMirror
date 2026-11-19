// PromotionStore (spec 4.5): reads the Win11 NotifyIconSettings key,
// matches entries to model icons and watches the key without polling.
// The registry half runs against a scratch key under HKCU, never the
// live one.
#include "core/PromotionStore.h"

#include "core/TrayModel.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace {

using traymirror::ApplyPromotion;
using traymirror::IconKey;
using traymirror::MatchesIcon;
using traymirror::NotifyIconSetting;
using traymirror::PromotionWatcher;
using traymirror::ReadNotifyIconSettings;
using traymirror::TrayIcon;
using traymirror::TrayModel;

constexpr wchar_t kScratchRoot[] = L"Software\\TrayMirrorTests\\PromotionStore";
constexpr wchar_t kSubkeyOne[] = L"Software\\TrayMirrorTests\\PromotionStore\\1";
constexpr wchar_t kSubkeyTwo[] = L"Software\\TrayMirrorTests\\PromotionStore\\2";
constexpr UINT kWmChanged = WM_APP + 61;
std::atomic<int> g_watcherHits{0};

// Creates the scratch tree fresh; each test starts from an empty store.
class PromotionStoreTest : public ::testing::Test {
 protected:
  void SetUp() override { DeleteTree(); }
  void TearDown() override { DeleteTree(); }

  static void DeleteTree() {
    RegDeleteTreeW(HKEY_CURRENT_USER, kScratchRoot);
  }

  static void WriteEntry(const wchar_t* subkey, const wchar_t* exePath,
                         DWORD uid, DWORD promoted) {
    HKEY key = nullptr;
    ASSERT_EQ(RegCreateKeyExW(HKEY_CURRENT_USER, subkey, 0, nullptr, 0,
                              KEY_ALL_ACCESS, nullptr, &key, nullptr),
              ERROR_SUCCESS);
    ASSERT_EQ(RegSetValueExW(key, L"ExecutablePath", 0, REG_SZ,
                             reinterpret_cast<const BYTE*>(exePath),
                             static_cast<DWORD>((wcslen(exePath) + 1) *
                                                sizeof(wchar_t))),
              ERROR_SUCCESS);
    ASSERT_EQ(RegSetValueExW(key, L"UID", 0, REG_DWORD,
                             reinterpret_cast<const BYTE*>(&uid), sizeof(uid)),
              ERROR_SUCCESS);
    ASSERT_EQ(RegSetValueExW(key, L"IsPromoted", 0, REG_DWORD,
                             reinterpret_cast<const BYTE*>(&promoted),
                             sizeof(promoted)),
              ERROR_SUCCESS);
    RegCloseKey(key);
  }

  static HKEY OpenScratch() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kScratchRoot, 0, KEY_READ, &key) !=
        ERROR_SUCCESS) {
      return nullptr;
    }
    return key;
  }
};

TrayMessage MakeAdd(uint32_t hwnd, uint32_t uid, const wchar_t* tip) {
  TrayMessage msg = {};
  msg.nim = 0;
  msg.hwnd = hwnd;
  msg.uid = uid;
  msg.flags = 0x4;  // NIF_TIP
  wcscpy_s(msg.tip, 130, tip);
  return msg;
}

TEST_F(PromotionStoreTest, ParsesPromotedAndHiddenEntries) {
  WriteEntry(kSubkeyOne, L"C:\\apps\\one.exe", 5, 1);
  WriteEntry(kSubkeyTwo, L"C:\\apps\\two.exe", 6, 0);

  HKEY key = OpenScratch();
  ASSERT_NE(key, nullptr);
  const std::vector<NotifyIconSetting> settings =
      ReadNotifyIconSettings(key);
  RegCloseKey(key);

  ASSERT_EQ(settings.size(), 2U);
  // Subkey order is not guaranteed; find each entry by its path.
  const NotifyIconSetting* one = nullptr;
  const NotifyIconSetting* two = nullptr;
  for (const NotifyIconSetting& s : settings) {
    if (s.exePath == L"C:\\apps\\one.exe") one = &s;
    if (s.exePath == L"C:\\apps\\two.exe") two = &s;
  }
  ASSERT_NE(one, nullptr);
  ASSERT_NE(two, nullptr);
  EXPECT_EQ(one->uid, 5U);
  EXPECT_TRUE(one->isPromoted);
  EXPECT_EQ(two->uid, 6U);
  EXPECT_FALSE(two->isPromoted);
}

TEST_F(PromotionStoreTest, EntryWithoutPromotedValueStaysPromoted) {
  HKEY key = nullptr;
  ASSERT_EQ(RegCreateKeyExW(HKEY_CURRENT_USER, kSubkeyOne, 0, nullptr, 0,
                            KEY_ALL_ACCESS, nullptr, &key, nullptr),
            ERROR_SUCCESS);
  const wchar_t path[] = L"C:\\apps\\bare.exe";
  RegSetValueExW(key, L"ExecutablePath", 0, REG_SZ,
                 reinterpret_cast<const BYTE*>(path), sizeof(path));
  RegCloseKey(key);

  HKEY opened = OpenScratch();
  ASSERT_NE(opened, nullptr);
  const std::vector<NotifyIconSetting> settings =
      ReadNotifyIconSettings(opened);
  RegCloseKey(opened);

  ASSERT_EQ(settings.size(), 1U);
  EXPECT_TRUE(settings[0].isPromoted);  // the safe default
}

TEST_F(PromotionStoreTest, MatchesByExePathAndUid) {
  TrayIcon icon = {};
  icon.exePath = L"C:\\Apps\\One.exe";  // different case than the entry
  icon.key.uid = 5;

  NotifyIconSetting setting;
  setting.exePath = L"c:\\apps\\one.EXE";
  setting.uid = 5;
  setting.isPromoted = false;
  EXPECT_TRUE(MatchesIcon(setting, icon));

  setting.uid = 6;  // same executable, another instance's icon
  EXPECT_FALSE(MatchesIcon(setting, icon));

  setting.uid = 5;
  setting.exePath = L"C:\\apps\\other.exe";
  EXPECT_FALSE(MatchesIcon(setting, icon));
}

TEST_F(PromotionStoreTest, ApplyPromotionMovesOnlyMatchedIcons) {
  // Injected resolvers: both icons belong to the same executable, so the
  // uID is what tells their registry entries apart.
  TrayModel model(
      traymirror::DefaultIconOps(),
      [](HWND, DWORD* pid, DWORD* tid) {
        *pid = 10;
        *tid = 11;
        return true;
      },
      [](DWORD) { return std::wstring(L"C:\\apps\\one.exe"); });
  ASSERT_TRUE(model.Apply(MakeAdd(0x1234, 5, L"one")));
  ASSERT_TRUE(model.Apply(MakeAdd(0x5678, 6, L"two")));
  ASSERT_EQ(model.icons().size(), 2U);

  std::vector<NotifyIconSetting> settings;
  NotifyIconSetting hidden;
  hidden.exePath = L"c:\\APPS\\one.EXE";  // case-insensitive match
  hidden.uid = 5;
  hidden.isPromoted = false;
  settings.push_back(hidden);

  EXPECT_TRUE(ApplyPromotion(&model, settings));
  EXPECT_FALSE(model.icons()[0].promoted);
  EXPECT_TRUE(model.icons()[1].promoted);
  EXPECT_FALSE(ApplyPromotion(&model, settings));  // steady state
}

TEST_F(PromotionStoreTest, WatcherPostsOncePerChange) {
  WriteEntry(kSubkeyOne, L"C:\\apps\\one.exe", 5, 1);

  // A plain message-only window on this thread counts the notifications.
  struct Local {
    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wparam,
                                 LPARAM lparam) {
      if (msg == kWmChanged) {
        ++g_watcherHits;
        return 0;
      }
      return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
  };
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = &Local::Proc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"TrayMirrorPromotionWatcherTest";
  RegisterClassExW(&wc);
  const HWND window = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                                      HWND_MESSAGE, nullptr, wc.hInstance,
                                      nullptr);
  ASSERT_NE(window, nullptr);

  g_watcherHits = 0;
  PromotionWatcher watcher([]() { return OpenScratch(); });
  ASSERT_TRUE(watcher.Start(window, kWmChanged));

  // Pump the thread's queue while the watcher fires.
  const auto pump = [&](int ms) {
    const auto until = std::chrono::steady_clock::now() +
                       std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
      MSG msg;
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        DispatchMessageW(&msg);
      }
      Sleep(5);
    }
  };
  pump(200);
  EXPECT_EQ(g_watcherHits.load(), 0);

  WriteEntry(kSubkeyTwo, L"C:\\apps\\two.exe", 6, 0);
  pump(500);
  EXPECT_EQ(g_watcherHits.load(), 1);
  pump(300);  // no further change: no further notification
  EXPECT_EQ(g_watcherHits.load(), 1);

  watcher.Stop();
  DestroyWindow(window);
}

}  // namespace
