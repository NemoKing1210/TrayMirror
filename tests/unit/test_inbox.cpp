// ModelInbox (spec 2.4): the reader-to-UI hand-off. The nudge is a real
// posted message observed through a window procedure — no mocks.
#include "core/ModelInbox.h"

#include <windows.h>

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

namespace {

using traymirror::ModelInbox;

TrayMessage MakeMsg(uint32_t hwnd, uint32_t uid) {
  TrayMessage msg = {};
  msg.hwnd = hwnd;
  msg.uid = uid;
  return msg;
}

std::atomic<int> g_nudges{0};

LRESULT CALLBACK CountProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  if (msg == WM_APP + 7) {
    ++g_nudges;
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void PumpMessages() {
  MSG msg = {};
  while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE) != FALSE) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
}

}  // namespace

TEST(ModelInbox, PushAndDrainInOrder) {
  ModelInbox inbox;
  std::thread producer([&inbox]() {
    for (uint32_t i = 0; i < 3; ++i) {
      inbox.Push(MakeMsg(0x100 + i, i));
    }
  });
  producer.join();

  const std::vector<TrayMessage> got = inbox.Drain();
  ASSERT_EQ(got.size(), 3U);
  for (uint32_t i = 0; i < 3; ++i) {
    EXPECT_EQ(got[i].hwnd, 0x100 + i);
    EXPECT_EQ(got[i].uid, i);
  }
}

TEST(ModelInbox, DrainEmptiesTheQueue) {
  ModelInbox inbox;
  inbox.Push(MakeMsg(1, 1));
  EXPECT_EQ(inbox.Drain().size(), 1U);
  EXPECT_EQ(inbox.Drain().size(), 0U);
}

TEST(ModelInbox, PushPostsTheNudge) {
  WNDCLASSW wc = {};
  wc.lpfnWndProc = CountProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"TrayMirrorInboxTestWnd";
  ASSERT_NE(RegisterClassW(&wc), 0);
  HWND window =
      CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr,
                      nullptr, wc.hInstance, nullptr);
  ASSERT_NE(window, nullptr);

  g_nudges = 0;
  ModelInbox inbox(window, WM_APP + 7);
  inbox.Push(MakeMsg(1, 1));
  PumpMessages();
  EXPECT_EQ(g_nudges.load(), 1);

  // Without a target the nudge is silently skipped, never a crash.
  ModelInbox detached;
  detached.Push(MakeMsg(2, 2));
  EXPECT_EQ(detached.Drain().size(), 1U);

  DestroyWindow(window);
}
