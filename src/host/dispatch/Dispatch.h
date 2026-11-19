#pragma once

#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace traymirror {

// A user gesture on a mirrored icon (spec 4.4). One gesture maps to one
// or more callback messages replayed to the icon's owner.
enum class TrayEvent : uint8_t {
  kLeftDown,
  kLeftUp,
  kLeftDblClk,
  kRightDown,
  kRightUp,
  kMouseMove,
  kMouseLeave,
};

// The icon owner a callback must be replayed to. `version` is the
// NOTIFYICON_VERSION_* the owner registered with (0 = v0); stage 2
// replays the v0 callback form only (R2).
struct DispatchTarget {
  HWND owner = nullptr;
  UINT callbackMessage = 0;
  UINT uid = 0;
  UINT version = 0;
  DWORD ownerPid = 0;
};

struct DispatchCall {
  DispatchTarget target;
  TrayEvent event = TrayEvent::kLeftUp;
  POINT screenPos = {};
};

using DispatchSender = std::function<void(const DispatchCall&)>;
using ForegroundAllower = std::function<BOOL(DWORD pid)>;

// The production sender: posts the v0 callback message to the owner and
// lets the owner take the foreground before a menu opens. Mouse moves
// are coalesced to at most one per 50 ms per owner. An empty `allow`
// means AllowSetForegroundWindow.
DispatchSender DefaultDispatchSender(ForegroundAllower allow = {});

// One worker thread that replays queued gestures to icon owners, so a
// hung application never blocks the overlay's UI thread (spec 4.4).
// Destruction drains the queue: every accepted call is delivered.
class Dispatcher {
 public:
  explicit Dispatcher(DispatchSender sender = DefaultDispatchSender());
  ~Dispatcher();

  Dispatcher(const Dispatcher&) = delete;
  Dispatcher& operator=(const Dispatcher&) = delete;

  // Never blocks on the owner: it only enqueues.
  void Post(const DispatchCall& call);

 private:
  void Run();

  DispatchSender sender_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<DispatchCall> queue_;
  bool stopping_ = false;
  std::thread thread_;
};

}  // namespace traymirror
