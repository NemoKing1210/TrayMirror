// Registry promotion state (spec 4.5): Windows 11 records which tray
// icons the user promoted to the taskbar in
// HKCU\Control Panel\NotifyIconSettings\<id>. The overlay uses it to
// decide what belongs on the strip and what belongs in the overflow.
// Host-only code; the live key is read, never written.
#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace traymirror {

class TrayModel;
struct TrayIcon;

// One registry entry, reduced to what the model needs.
struct NotifyIconSetting {
  std::wstring exePath;  // ExecutablePath
  UINT uid = 0;          // UID
  bool isPromoted = false;  // missing IsPromoted is the shell's default overflow
  GUID guid = {};
  bool hasGuid = false;
  bool hasUid = true;
  UINT order = UINT_MAX;
};

// Reads shell identity, visibility and validated UIOrderList placement.
// Missing promotion values remain accessible in the overflow panel.
std::vector<NotifyIconSetting> ReadNotifyIconSettings(HKEY settings);

// Opens HKCU\Control Panel\NotifyIconSettings for reading. Returns
// nullptr when the key does not exist (older builds, restricted profile).
HKEY OpenLiveNotifyIconSettings();

// The registry entry that belongs to `icon`: the executable path is
// canonicalized and compared case-insensitively, with an exact uID.
// GUID registrations match by IconGuid, never just by executable path.
bool MatchesIcon(const NotifyIconSetting& setting, const TrayIcon& icon);

// Applies the whole store to the model. Returns true iff the visible
// model changed (an icon moved between the strip and the overflow).
bool ApplyPromotion(TrayModel* model,
                    const std::vector<NotifyIconSetting>& settings);

// Watches the settings key without polling: RegNotifyChangeKeyValue on a
// worker thread, one posted message per change (spec 2.3.6).
class PromotionWatcher {
 public:
  // `open` returns a freshly opened key handle or nullptr when the key is
  // unavailable; the watcher closes the handle it opened for itself.
  using KeyOpener = std::function<HKEY()>;

  explicit PromotionWatcher(KeyOpener open);
  ~PromotionWatcher();

  PromotionWatcher(const PromotionWatcher&) = delete;
  PromotionWatcher& operator=(const PromotionWatcher&) = delete;

  // Returns false when the key cannot be opened; the caller then runs
  // without registry promotion (every icon stays promoted).
  bool Start(HWND notify, UINT message);
  void Stop();

 private:
  void Run();

  KeyOpener open_;
  HWND notify_ = nullptr;
  UINT message_ = 0;
  HANDLE stop_ = nullptr;
  HANDLE changed_ = nullptr;
  HANDLE ready_ = nullptr;
  std::thread thread_;
};

}  // namespace traymirror
