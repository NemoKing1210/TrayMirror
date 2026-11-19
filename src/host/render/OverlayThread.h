// The UI thread (spec 2.4/6.1): owns every overlay window and the tray
// model. The reader thread never touches either — it pushes into the
// inbox and posts kMsgInbox to uiHwnd(). Event-driven only: no timers.
#pragma once

#include "core/ModelInbox.h"
#include "core/PromotionStore.h"
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

// Posted by the promotion watcher when the registry store changes; the
// UI thread re-reads the store and applies it to the model.
inline constexpr UINT kMsgPromotion = WM_APP + 3;

class OverlayThread {
 public:
  OverlayThread() = default;
  ~OverlayThread();
  OverlayThread(const OverlayThread&) = delete;
  OverlayThread& operator=(const OverlayThread&) = delete;

  // Starts the UI thread and returns once the message window exists
  // (uiHwnd() is valid on return). False when the window could not be
  // created; in that case nothing is running. `sender` carries every
  // gesture on a mirrored icon off the UI thread (spec 4.4). `promotion`
  // opens the registry promotion store; tests pass an opener that
  // returns nullptr so the live store stays out of the test.
  bool Start(ModelInbox* inbox, TrayModel* model,
             MonitorEnumerator enumerate = DefaultEnumerateSecondaryMonitors,
             DispatchSender sender = {},
             PromotionWatcher::KeyOpener promotion =
                 OpenLiveNotifyIconSettings);
  void Stop();
  [[nodiscard]] HWND uiHwnd() const noexcept { return uiHwnd_.load(); }

 private:
  void Run();
  void BuildOverlays();
  void ApplyInbox();
  // Re-reads the registry promotion store and applies it to the model.
  // Returns true when an icon moved between the strip and the overflow.
  bool ApplyPromotionFromStore();
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
  PromotionWatcher::KeyOpener promotion_;
  // Created on the UI thread once the sender is known; destroyed with it.
  std::unique_ptr<Dispatcher> dispatcher_;
  // Registry promotion (spec 4.5): event-driven, no polling. A null
  // watcher means the store is unavailable and every icon stays on the
  // strip.
  std::unique_ptr<PromotionWatcher> watcher_;
  // The icon count the promotion store was last applied against; a new
  // icon needs its registry entry read, a modify does not.
  size_t promotionIconCount_ = 0;
  std::vector<std::unique_ptr<Overlay>> overlays_;
  // How many icon slots the current strips were built for, and whether
  // the taskbar had no room for that many (then growth cannot help).
  size_t builtSlots_ = 0;
  bool stripRoomExhausted_ = false;
};

}  // namespace traymirror
