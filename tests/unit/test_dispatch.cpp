// Dispatcher (spec 4.4): the worker that replays tray callbacks to icon
// owners. A real receiver window on its own thread records what arrives;
// the sequence-pinned test lands with the S4 measurement (report 07).
#include "dispatch/Dispatch.h"

#include <windows.h>

#include <gtest/gtest.h>

#include <atomic>
#include <bit>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using traymirror::DispatchCall;
using traymirror::Dispatcher;
using traymirror::TrayEvent;

constexpr UINT kCallback = WM_APP + 100;
constexpr UINT kHangMessage = WM_APP + 101;
constexpr wchar_t kReceiverClass[] = L"TrayMirrorDispatchTestReceiver";

// A tray icon owner: its own thread and window, recording every callback.
class Receiver {
 public:
  bool Start() {
    ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ready_ == nullptr) {
      return false;
    }
    thread_ = std::thread([this]() { Run(); });
    WaitForSingleObject(ready_, 5000);
    CloseHandle(ready_);
    ready_ = nullptr;
    return hwnd_.load() != nullptr;
  }

  void Stop() {
    if (!thread_.joinable()) {
      return;
    }
    PostMessageW(hwnd_.load(), WM_CLOSE, 0, 0);
    thread_.join();
  }

  ~Receiver() { Stop(); }

  [[nodiscard]] HWND hwnd() const noexcept { return hwnd_.load(); }

  [[nodiscard]] size_t Total() const {
    const std::scoped_lock lock(mutex_);
    return received_;
  }

  std::vector<UINT> DrainEvents() {
    const std::scoped_lock lock(mutex_);
    std::vector<UINT> out;
    out.swap(events_);
    return out;
  }

  // Blocks the owner's own message loop, like an application that stopped
  // pumping. Returns once the owner thread has entered the block.
  void Hang(DWORD ms) {
    hung_ = false;
    PostMessageW(hwnd_.load(), kHangMessage, ms, 0);
    for (int waited = 0; waited < 2000 && !hung_; waited += 5) {
      Sleep(5);
    }
  }

 private:
  void Run() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &Receiver::Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kReceiverClass;
    RegisterClassExW(&wc);
    const HWND hwnd = CreateWindowExW(0, kReceiverClass, L"", WS_POPUP, 0, 0, 0,
                                      0, nullptr, nullptr, wc.hInstance, this);
    hwnd_.store(hwnd);
    SetEvent(ready_);
    if (hwnd == nullptr) {
      return;
    }
    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }

  static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wparam,
                               LPARAM lparam) {
    auto* self = std::bit_cast<Receiver*>(
        static_cast<LONG_PTR>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)));
    if (message == WM_NCCREATE) {
      const auto* create = std::bit_cast<CREATESTRUCTW*>(lparam);
      self = static_cast<Receiver*>(create->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, std::bit_cast<LONG_PTR>(self));
    }
    if (message == WM_CLOSE) {
      DestroyWindow(hwnd);
      return 0;
    }
    if (message == WM_DESTROY) {
      PostQuitMessage(0);
      return 0;
    }
    if (self == nullptr) {
      return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    if (message == kHangMessage) {
      self->hung_ = true;
      Sleep(static_cast<DWORD>(wparam));
      return 0;
    }
    if (message == kCallback) {
      const std::scoped_lock lock(self->mutex_);
      self->events_.push_back(static_cast<UINT>(lparam) & 0xFFFFU);
      ++self->received_;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
  }

  std::thread thread_;
  HANDLE ready_ = nullptr;
  std::atomic<HWND> hwnd_{nullptr};
  std::atomic<bool> hung_{false};
  mutable std::mutex mutex_;
  std::vector<UINT> events_;
  size_t received_ = 0;
};

DispatchCall MakeCall(HWND owner, TrayEvent event) {
  DispatchCall call;
  call.target.owner = owner;
  call.target.callbackMessage = kCallback;
  call.target.uid = 7;
  call.target.ownerPid = 4242;
  call.event = event;
  return call;
}

bool WaitForTotal(Receiver* receiver, size_t count, int timeoutMs = 2000) {
  for (int waited = 0; waited < timeoutMs; waited += 10) {
    if (receiver->Total() >= count) {
      return true;
    }
    Sleep(10);
  }
  return false;
}

}  // namespace

TEST(DispatchTest, V0RightClickAllowsForeground) {
  Receiver receiver;
  ASSERT_TRUE(receiver.Start());

  std::atomic<DWORD> allowedPid{0};
  std::atomic<int> allowedCount{0};
  const auto sender = traymirror::DefaultDispatchSender([&](DWORD pid) {
    allowedPid = pid;
    ++allowedCount;
    return TRUE;
  });
  {
    Dispatcher dispatcher(sender);
    dispatcher.Post(MakeCall(receiver.hwnd(), TrayEvent::kRightUp));
    ASSERT_TRUE(WaitForTotal(&receiver, 1));
  }
  EXPECT_EQ(allowedCount.load(), 1);
  EXPECT_EQ(allowedPid.load(), 4242U);
  const std::vector<UINT> events = receiver.DrainEvents();
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0], WM_RBUTTONUP);
}

TEST(DispatchTest, PostDoesNotBlockOnHungOwner) {
  Receiver receiver;
  ASSERT_TRUE(receiver.Start());
  receiver.Hang(300);  // the owner has stopped pumping

  Dispatcher dispatcher;
  const auto start = std::chrono::steady_clock::now();
  dispatcher.Post(MakeCall(receiver.hwnd(), TrayEvent::kLeftUp));
  const auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
                .count(),
            50);
  // Delivery still happens once the owner pumps again.
  EXPECT_TRUE(WaitForTotal(&receiver, 1));
}

TEST(DispatchTest, MouseMoveDebounced) {
  Receiver receiver;
  ASSERT_TRUE(receiver.Start());

  Dispatcher dispatcher;
  dispatcher.Post(MakeCall(receiver.hwnd(), TrayEvent::kMouseMove));
  dispatcher.Post(MakeCall(receiver.hwnd(), TrayEvent::kMouseMove));
  ASSERT_TRUE(WaitForTotal(&receiver, 1));
  Sleep(80);  // past the 50 ms window
  dispatcher.Post(MakeCall(receiver.hwnd(), TrayEvent::kMouseMove));
  ASSERT_TRUE(WaitForTotal(&receiver, 2));

  const std::vector<UINT> events = receiver.DrainEvents();
  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(events[0], WM_MOUSEMOVE);
  EXPECT_EQ(events[1], WM_MOUSEMOVE);
}

TEST(DispatchTest, DestructorDrainsQueue) {
  Receiver receiver;
  ASSERT_TRUE(receiver.Start());

  {
    Dispatcher dispatcher;
    dispatcher.Post(MakeCall(receiver.hwnd(), TrayEvent::kLeftDown));
    dispatcher.Post(MakeCall(receiver.hwnd(), TrayEvent::kLeftUp));
    dispatcher.Post(MakeCall(receiver.hwnd(), TrayEvent::kRightDown));
  }
  // After the dispatcher is gone, every queued call must have been sent.
  EXPECT_TRUE(WaitForTotal(&receiver, 3));
  EXPECT_EQ(receiver.DrainEvents().size(), 3U);
}
