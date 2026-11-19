// Dead owner windows (spec 4.4): a tray icon belongs to a window, and a
// window that is destroyed takes its icons with it. Explorer tells the
// model nothing, so the host watches EVENT_OBJECT_DESTROY.
//
// The hook is WINEVENT_OUTOFCONTEXT: it runs on the thread that installed
// it, in this process, and never injects into another one. One watcher
// per process — the callback needs to find its owner.
#pragma once

#include <windows.h>

#include <functional>

namespace traymirror {

class DeadIconWatcher {
 public:
  // Runs on the installing thread's message loop for every destroyed
  // top-level window in the session; the model ignores unknown owners.
  using OwnerGone = std::function<void(HWND)>;

  explicit DeadIconWatcher(OwnerGone onGone);
  ~DeadIconWatcher();
  DeadIconWatcher(const DeadIconWatcher&) = delete;
  DeadIconWatcher& operator=(const DeadIconWatcher&) = delete;

  // Requires a message loop on the calling thread, which is what
  // delivers the events. False when the hook could not be installed.
  bool Start();
  void Stop();

  [[nodiscard]] bool running() const noexcept { return hook_ != nullptr; }

 private:
  static void CALLBACK EventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                 LONG idObject, LONG idChild, DWORD thread,
                                 DWORD time);

  OwnerGone onGone_;
  HWINEVENTHOOK hook_ = nullptr;
};

}  // namespace traymirror
