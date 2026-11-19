// Dead owner windows (spec 4.4): an icon whose window is destroyed can
// never be clicked again, so the model must drop it — from the WinEvent
// hook when the window dies, and lazily before a gesture is replayed.
#include "core/DeadIconWatcher.h"
#include "core/TrayModel.h"

#include "TrayPayload.h"

#include <windows.h>

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

namespace {

using traymirror::DeadIconWatcher;
using traymirror::SignExtendHwnd;
using traymirror::TrayModel;

constexpr wchar_t kOwnerClass[] = L"TrayMirrorDeadIconTestOwner";
constexpr uint32_t kNimAdd = 0;
constexpr uint32_t kNifIcon = 0x2;

// A real window in this process; the model resolves its executable from
// it, and destroying it is what the hook has to notice.
HWND CreateOwnerWindow() {
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = kOwnerClass;
  RegisterClassExW(&wc);
  return CreateWindowExW(0, kOwnerClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                         nullptr, wc.hInstance, nullptr);
}

TrayMessage MakeAddFor(HWND owner, uint32_t uid) {
  TrayMessage msg = {};
  msg.nim = kNimAdd;
  msg.hwnd = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(owner));
  msg.uid = uid;
  msg.flags = kNifIcon;
  msg.icon = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(
      LoadIconW(nullptr, MAKEINTRESOURCEW(32512))));
  wcscpy_s(msg.tip, 130, L"dead owner");
  return msg;
}

}  // namespace

TEST(DeadIcons, DestroyedOwnerIsRemoved) {
  const HWND owner = CreateOwnerWindow();
  ASSERT_NE(owner, nullptr);
  TrayModel model;
  ASSERT_TRUE(model.Apply(MakeAddFor(owner, 1)));
  ASSERT_EQ(model.icons().size(), 1U);

  DestroyWindow(owner);

  EXPECT_TRUE(model.RemoveIfOwnerGone(owner));
  EXPECT_TRUE(model.icons().empty());
}

TEST(DeadIcons, LiveOwnerSurvivesTheLazyCheck) {
  const HWND owner = CreateOwnerWindow();
  ASSERT_NE(owner, nullptr);
  TrayModel model;
  ASSERT_TRUE(model.Apply(MakeAddFor(owner, 1)));

  EXPECT_FALSE(model.RemoveIfOwnerGone(owner));
  EXPECT_EQ(model.icons().size(), 1U);

  DestroyWindow(owner);
}

// The hook is what removes the icon without a gesture: the window dies,
// the watcher reports it on this thread's message loop, the model drops
// the icon.
TEST(DeadIcons, WinEventHookRemovesTheOwner) {
  TrayModel model;
  DeadIconWatcher watcher([&model](HWND owner) { model.RemoveOwner(owner); });
  ASSERT_TRUE(watcher.Start());

  const HWND owner = CreateOwnerWindow();
  ASSERT_NE(owner, nullptr);
  ASSERT_TRUE(model.Apply(MakeAddFor(owner, 1)));
  ASSERT_EQ(model.icons().size(), 1U);

  DestroyWindow(owner);
  // Bounded pump: the out-of-context event arrives on this thread's
  // queue, so it is processed here.
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
  while (std::chrono::steady_clock::now() < deadline && !model.icons().empty()) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      DispatchMessageW(&msg);
    }
    Sleep(5);
  }
  EXPECT_TRUE(model.icons().empty());
  watcher.Stop();
}

// The hook sees every window in the session: windows that own no icon
// must not disturb the model.
TEST(DeadIcons, UnrelatedWindowDoesNotRemoveAnIcon) {
  TrayModel model;
  const HWND owner = CreateOwnerWindow();
  ASSERT_NE(owner, nullptr);
  ASSERT_TRUE(model.Apply(MakeAddFor(owner, 1)));

  DeadIconWatcher watcher([&model](HWND gone) { model.RemoveOwner(gone); });
  ASSERT_TRUE(watcher.Start());
  const HWND unrelated = CreateWindowExW(0, kOwnerClass, L"", 0, 0, 0, 0, 0,
                                         HWND_MESSAGE, nullptr,
                                         GetModuleHandleW(nullptr), nullptr);
  ASSERT_NE(unrelated, nullptr);
  DestroyWindow(unrelated);

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
  while (std::chrono::steady_clock::now() < deadline) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      DispatchMessageW(&msg);
    }
    Sleep(5);
  }
  EXPECT_EQ(model.icons().size(), 1U);
  EXPECT_EQ(model.icons()[0].ownerHwnd, owner);

  watcher.Stop();
  DestroyWindow(owner);
}
