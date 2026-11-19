#include "core/DeadIconWatcher.h"

#include <utility>

namespace traymirror {
namespace {

// The WinEvent callback carries the hook handle but no context, so the
// single process watcher is found here. A second watcher replaces the
// first; the host has exactly one.
DeadIconWatcher* g_watcher = nullptr;

}  // namespace

DeadIconWatcher::DeadIconWatcher(OwnerGone onGone)
    : onGone_(std::move(onGone)) {}

DeadIconWatcher::~DeadIconWatcher() { Stop(); }

bool DeadIconWatcher::Start() {
  if (hook_ != nullptr || onGone_ == nullptr) {
    return false;
  }
  g_watcher = this;
  hook_ = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_DESTROY, nullptr,
                          &DeadIconWatcher::EventProc, 0, 0,
                          WINEVENT_OUTOFCONTEXT);
  if (hook_ == nullptr) {
    g_watcher = nullptr;
    return false;
  }
  return true;
}

void DeadIconWatcher::Stop() {
  if (hook_ == nullptr) {
    return;
  }
  UnhookWinEvent(hook_);
  hook_ = nullptr;
  if (g_watcher == this) {
    g_watcher = nullptr;
  }
}

void CALLBACK DeadIconWatcher::EventProc(HWINEVENTHOOK /*hook*/, DWORD event,
                                         HWND hwnd, LONG idObject,
                                         LONG idChild, DWORD /*thread*/,
                                         DWORD /*time*/) {
  // Every destroyed window in the session arrives here; only a window
  // object that became invalid is an owner we can act on.
  if (g_watcher == nullptr || event != EVENT_OBJECT_DESTROY ||
      hwnd == nullptr || idObject != OBJID_WINDOW ||
      idChild != CHILDID_SELF) {
    return;
  }
  g_watcher->onGone_(hwnd);
}

}  // namespace traymirror
