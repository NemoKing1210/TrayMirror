// The reader-to-UI hand-off (spec 2.4): the reader thread pushes decoded
// records, then nudges the UI window; the UI thread drains them in its
// message handler. The queue is a mutex-protected vector — one push per
// tray record, no allocation inside the hook.
#pragma once

#include "TrayPayload.h"

#include <windows.h>

#include <mutex>
#include <vector>

namespace traymirror {

class ModelInbox {
 public:
  ModelInbox() = default;
  ModelInbox(HWND nudge, UINT message);
  ~ModelInbox() = default;
  ModelInbox(const ModelInbox&) = delete;
  ModelInbox& operator=(const ModelInbox&) = delete;

  // Installs the nudge target once the UI window exists (it cannot be
  // known when the host constructs the inbox). A null target disables
  // the nudge; the queue still works.
  void SetNudge(HWND nudge, UINT message);

  void Push(const TrayMessage& msg);
  std::vector<TrayMessage> Drain();

 private:
  std::mutex mutex_;
  std::vector<TrayMessage> queue_;
  HWND nudge_ = nullptr;  // written before the reader starts; read-only after
  UINT message_ = 0;
};

}  // namespace traymirror
