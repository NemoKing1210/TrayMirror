#include "dispatch/Dispatch.h"

#include <map>
#include <memory>
#include <utility>

namespace traymirror {
namespace {

// Coalesces hover traffic: an icon owner gets at most one move per window.
constexpr ULONGLONG kMouseMoveDebounceMs = 50;

// v0 callback form: wParam is the icon id, lParam low word the event.
// Returns false when the gesture has no v0 callback.
bool V0MessageFor(TrayEvent event, UINT* message) {
  switch (event) {
    case TrayEvent::kLeftDown:
      *message = WM_LBUTTONDOWN;
      return true;
    case TrayEvent::kLeftUp:
      *message = WM_LBUTTONUP;
      return true;
    case TrayEvent::kLeftDblClk:
      *message = WM_LBUTTONDBLCLK;
      return true;
    case TrayEvent::kRightDown:
      *message = WM_RBUTTONDOWN;
      return true;
    case TrayEvent::kRightUp:
      *message = WM_RBUTTONUP;
      return true;
    case TrayEvent::kMouseMove:
      *message = WM_MOUSEMOVE;
      return true;
    case TrayEvent::kMouseLeave:
      return false;  // v0 has no leave callback
  }
  return false;
}

}  // namespace

DispatchSender DefaultDispatchSender(ForegroundAllower allow) {
  ForegroundAllower allower =
      allow != nullptr ? std::move(allow) : ForegroundAllower([](DWORD pid) {
        return AllowSetForegroundWindow(pid);
      });
  // Only the worker thread touches the debounce table.
  auto lastMove = std::make_shared<std::map<HWND, ULONGLONG>>();
  return [allower, lastMove](const DispatchCall& call) {
    UINT message = 0;
    if (!V0MessageFor(call.event, &message)) {
      return;
    }
    if (call.target.owner == nullptr) {
      return;
    }
    if (call.event == TrayEvent::kMouseMove) {
      const ULONGLONG now = GetTickCount64();
      ULONGLONG& last = (*lastMove)[call.target.owner];
      if (last != 0 && now - last < kMouseMoveDebounceMs) {
        return;
      }
      last = now;
    }
    if (call.event == TrayEvent::kRightUp) {
      // The owner is about to show a menu; without this its window would
      // not be allowed to come to the foreground.
      allower(call.target.ownerPid);
    }
    PostMessageW(call.target.owner, call.target.callbackMessage,
                 call.target.uid, static_cast<LPARAM>(message));
  };
}

Dispatcher::Dispatcher(DispatchSender sender)
    : sender_(std::move(sender)), thread_([this]() { Run(); }) {}

Dispatcher::~Dispatcher() {
  {
    const std::scoped_lock lock(mutex_);
    stopping_ = true;
  }
  wake_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
}

void Dispatcher::Post(const DispatchCall& call) {
  {
    const std::scoped_lock lock(mutex_);
    queue_.push_back(call);
  }
  wake_.notify_one();
}

void Dispatcher::Run() {
  std::unique_lock lock(mutex_);
  for (;;) {
    wake_.wait(lock, [this]() { return !queue_.empty() || stopping_; });
    if (queue_.empty()) {
      break;  // stopping and drained
    }
    std::deque<DispatchCall> batch;
    batch.swap(queue_);
    lock.unlock();
    for (const DispatchCall& call : batch) {
      sender_(call);
    }
    lock.lock();
  }
}

}  // namespace traymirror
