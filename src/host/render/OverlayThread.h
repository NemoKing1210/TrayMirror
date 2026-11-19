// The UI thread (spec 2.4/6.1): owns every overlay window and the tray
// model. The reader thread never touches either — it pushes into the
// inbox and posts kMsgInbox to uiHwnd(). Event-driven only: no timers.
#pragma once

#include "core/ModelInbox.h"
#include "core/TrayModel.h"
#include "dispatch/Dispatch.h"
#include "render/Overlay.h"
#include "win/Monitors.h"

#include <windows.h>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

namespace traymirror {

// Posted by ModelInbox::Push to wake the UI thread.
inline constexpr UINT kMsgInbox = WM_APP + 1;

class OverlayThread {
 public:
  OverlayThread() = default;
  ~OverlayThread();
  OverlayThread(const OverlayThread&) = delete;
  OverlayThread& operator=(const OverlayThread&) = delete;

  // Starts the UI thread and returns once the message window exists
  // (uiHwnd() is valid on return). False when the window could not be
  // created; in that case nothing is running. `sender` carries every
  // gesture on a mirrored icon off the UI thread (spec 4.4).
  bool Start(ModelInbox* inbox, TrayModel* model,
             MonitorEnumerator enumerate = DefaultEnumerateSecondaryMonitors,
             DispatchSender sender = {});
  void Stop();
  [[nodiscard]] HWND uiHwnd() const noexcept { return uiHwnd_.load(); }

 private:
  void Run();
  void BuildOverlays();
  void ApplyInbox();
  void PushModelToOverlays();
  [[nodiscard]] size_t CountVisibleIcons() const;
  void PostGesture(const DispatchCall& call);

  static bool EnsureUiClass();
  static LRESULT CALLBACK UiProc(HWND hwnd, UINT message, WPARAM wparam,
                                 LPARAM lparam);
  LRESULT HandleUiMessage(HWND hwnd, UINT message, WPARAM wparam,
                          LPARAM lparam);

  std::thread thread_;
  HANDLE ready_ = nullptr;
  std::atomic<HWND> uiHwnd_{nullptr};
  std::atomic<DWORD> threadId_{0};
  ModelInbox* inbox_ = nullptr;
  TrayModel* model_ = nullptr;
  MonitorEnumerator enumerate_;
  DispatchSender sender_;
  // Created on the UI thread once the sender is known; destroyed with it.
  std::unique_ptr<Dispatcher> dispatcher_;
  std::vector<std::unique_ptr<Overlay>> overlays_;
  // How many icon slots the current strips were built for, and whether
  // the taskbar had no room for that many (then growth cannot help).
  size_t builtSlots_ = 0;
  bool stripRoomExhausted_ = false;
};

}  // namespace traymirror
