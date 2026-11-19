#include "core/PromotionStore.h"

#include "core/TrayModel.h"

namespace traymirror {
namespace {

constexpr wchar_t kSettingsKey[] = L"Control Panel\\NotifyIconSettings";
constexpr wchar_t kValueExePath[] = L"ExecutablePath";
constexpr wchar_t kValueUid[] = L"UID";
constexpr wchar_t kValuePromoted[] = L"IsPromoted";

std::wstring ReadString(HKEY key, const wchar_t* name) {
  DWORD type = 0;
  DWORD bytes = 0;
  if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) !=
          ERROR_SUCCESS ||
      (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t)) {
    return {};
  }
  std::wstring value(bytes / sizeof(wchar_t), L'\0');
  if (RegQueryValueExW(key, name, nullptr, &type,
                       reinterpret_cast<BYTE*>(value.data()), &bytes) !=
      ERROR_SUCCESS) {
    return {};
  }
  value.resize(wcslen(value.c_str()));  // the value is NUL-terminated
  return value;
}

bool ReadDword(HKEY key, const wchar_t* name, DWORD* out) {
  DWORD type = 0;
  DWORD bytes = sizeof(DWORD);
  return RegQueryValueExW(key, name, nullptr, &type,
                          reinterpret_cast<BYTE*>(out), &bytes) ==
             ERROR_SUCCESS &&
         type == REG_DWORD;
}

bool SamePath(const std::wstring& a, const std::wstring& b) {
  return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(),
                              static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

}  // namespace

std::vector<NotifyIconSetting> ReadNotifyIconSettings(HKEY settings) {
  std::vector<NotifyIconSetting> out;
  if (settings == nullptr) {
    return out;
  }
  for (DWORD index = 0;; ++index) {
    wchar_t name[256] = {};
    auto length = static_cast<DWORD>(std::size(name));
    const LSTATUS status =
        RegEnumKeyExW(settings, index, name, &length, nullptr, nullptr,
                      nullptr, nullptr);
    if (status == ERROR_NO_MORE_ITEMS) {
      break;
    }
    if (status != ERROR_SUCCESS) {
      break;  // a partially readable key must not fail the whole store
    }
    HKEY entry = nullptr;
    if (RegOpenKeyExW(settings, name, 0, KEY_READ, &entry) != ERROR_SUCCESS) {
      continue;
    }
    NotifyIconSetting setting;
    setting.exePath = ReadString(entry, kValueExePath);
    DWORD uid = 0;
    setting.uid = ReadDword(entry, kValueUid, &uid) ? uid : 0;
    DWORD promoted = 1;
    setting.isPromoted =
        !ReadDword(entry, kValuePromoted, &promoted) || promoted != 0;
    RegCloseKey(entry);
    if (!setting.exePath.empty()) {
      out.push_back(std::move(setting));
    }
  }
  return out;
}

HKEY OpenLiveNotifyIconSettings() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, KEY_READ, &key) !=
      ERROR_SUCCESS) {
    return nullptr;
  }
  return key;
}

bool MatchesIcon(const NotifyIconSetting& setting, const TrayIcon& icon) {
  if (icon.exePath.empty() || setting.exePath.empty()) {
    return false;
  }
  if (!SamePath(setting.exePath, icon.exePath)) {
    return false;
  }
  // A GUID-keyed icon has no uID identity of its own: other instances of
  // the same executable pick their entries by uID, and a GUID icon would
  // otherwise swallow the first one.
  if (icon.key.kind == IconKey::Kind::kGuid) {
    return true;
  }
  return setting.uid == icon.key.uid;
}

bool ApplyPromotion(TrayModel* model,
                    const std::vector<NotifyIconSetting>& settings) {
  if (model == nullptr) {
    return false;
  }
  bool changed = false;
  for (const TrayIcon& icon : model->icons()) {
    bool promoted = true;  // an icon with no entry stays on the strip
    for (const NotifyIconSetting& setting : settings) {
      if (MatchesIcon(setting, icon)) {
        promoted = setting.isPromoted;
        break;
      }
    }
    if (model->SetPromotion(icon.key, promoted)) {
      changed = true;
    }
  }
  return changed;
}

PromotionWatcher::PromotionWatcher(KeyOpener open) : open_(std::move(open)) {}

PromotionWatcher::~PromotionWatcher() { Stop(); }

bool PromotionWatcher::Start(HWND notify, UINT message) {
  if (thread_.joinable() || open_ == nullptr || notify == nullptr) {
    return false;
  }
  HKEY probe = open_();
  if (probe == nullptr) {
    return false;
  }
  RegCloseKey(probe);
  notify_ = notify;
  message_ = message;
  stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  changed_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (stop_ == nullptr || changed_ == nullptr || ready_ == nullptr) {
    Stop();
    return false;
  }
  thread_ = std::thread(&PromotionWatcher::Run, this);
  WaitForSingleObject(ready_, 5000);
  return true;
}

void PromotionWatcher::Stop() {
  if (stop_ != nullptr) {
    SetEvent(stop_);
  }
  if (thread_.joinable()) {
    thread_.join();
  }
  for (HANDLE* handle : {&stop_, &changed_, &ready_}) {
    if (*handle != nullptr) {
      CloseHandle(*handle);
      *handle = nullptr;
    }
  }
}

void PromotionWatcher::Run() {
  // RegNotifyChangeKeyValue reports one change per registration, so the
  // loop re-arms after every signal. A single promotion toggle writes
  // several values in a row, hence the quiet window: the burst becomes
  // one message instead of one per value written.
  constexpr DWORD kQuietMs = 50;
  constexpr ULONGLONG kCoalesceMaxMs = 500;
  const HANDLE waits[2] = {stop_, changed_};
  HKEY key = open_();
  SetEvent(ready_);
  while (key != nullptr) {
    ResetEvent(changed_);
    if (RegNotifyChangeKeyValue(key, TRUE, REG_NOTIFY_CHANGE_LAST_SET, changed_,
                                TRUE) != ERROR_SUCCESS) {
      break;  // the key vanished: stop watching, keep the last state
    }
    if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) !=
        WAIT_OBJECT_0 + 1) {
      break;  // stop requested
    }
    const ULONGLONG deadline = GetTickCount64() + kCoalesceMaxMs;
    bool stop = false;
    bool armed = true;
    for (;;) {
      ResetEvent(changed_);
      if (RegNotifyChangeKeyValue(key, TRUE, REG_NOTIFY_CHANGE_LAST_SET,
                                  changed_, TRUE) != ERROR_SUCCESS) {
        armed = false;  // the key vanished mid-burst
        break;
      }
      const DWORD which = WaitForMultipleObjects(2, waits, FALSE, kQuietMs);
      if (which == WAIT_OBJECT_0) {
        stop = true;
        break;
      }
      if (which != WAIT_OBJECT_0 + 1) {
        break;  // the quiet window ended the burst
      }
      if (GetTickCount64() >= deadline) {
        break;  // a continuous writer must not starve the consumer
      }
    }
    if (stop) {
      break;
    }
    PostMessageW(notify_, message_, 0, 0);
    if (!armed) {
      break;
    }
  }
  if (key != nullptr) {
    RegCloseKey(key);
  }
}

}  // namespace traymirror
