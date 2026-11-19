#include "dispatch/Dispatch.h"

#include <shellapi.h>

#include <map>
#include <memory>
#include <utility>

namespace traymirror {
namespace {

// Coalesces hover traffic: an icon owner gets at most one move per window.
constexpr ULONGLONG kMouseMoveDebounceMs = 50;

// v4 callback form (report 07): wParam carries the screen point of the
// icon, lParam is MAKELPARAM(event, uid). One gesture maps to the
// measured message sequence.
constexpr int kV4MaxSteps = 2;

// Returns the number of messages the gesture produces, 0 when the
// gesture has no callback in this form.
int V4SequenceFor(TrayEvent event, UINT* events) {
  switch (event) {
    case TrayEvent::kLeftDown:
      events[0] = WM_LBUTTONDOWN;
      return 1;
    case TrayEvent::kLeftUp:
      // Every button-up is followed by exactly one NIN_SELECT (15/15).
      events[0] = WM_LBUTTONUP;
      events[1] = NIN_SELECT;
      return 2;
    case TrayEvent::kLeftDblClk:
      events[0] = WM_LBUTTONDBLCLK;
      return 1;
    case TrayEvent::kRightDown:
      events[0] = WM_RBUTTONDOWN;
      return 1;
    case TrayEvent::kRightUp:
      // A right-up is followed by WM_CONTEXTMENU (7/7).
      events[0] = WM_RBUTTONUP;
      events[1] = WM_CONTEXTMENU;
      return 2;
    case TrayEvent::kMouseMove:
      events[0] = WM_MOUSEMOVE;
      return 1;
    case TrayEvent::kMouseEnter:
      events[0] = NIN_POPUPOPEN;
      return 1;
    case TrayEvent::kMouseLeave:
      events[0] = NIN_POPUPCLOSE;
      return 1;
  }
  return 0;
}

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
    case TrayEvent::kMouseEnter:
      // v0 has neither a leave nor an enter callback (measured in S4).
      return false;
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
    if (call.target.version >= NOTIFYICON_VERSION_4) {
      UINT events[kV4MaxSteps] = {};
      const int count = V4SequenceFor(call.event, events);
      const WPARAM point = MAKELPARAM(static_cast<WORD>(call.screenPos.x),
                                      static_cast<WORD>(call.screenPos.y));
      for (int i = 0; i < count; ++i) {
        PostMessageW(call.target.owner, call.target.callbackMessage, point,
                     MAKELPARAM(static_cast<WORD>(events[i]),
                                static_cast<WORD>(call.target.uid)));
      }
      return;
    }
    UINT message = 0;
    if (!V0MessageFor(call.event, &message)) {
      return;
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
