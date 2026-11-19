#include "core/PromotionStore.h"

#include "core/TrayModel.h"

#include <shlobj.h>
#include <algorithm>
#include <cstring>
#include <memory>

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
      (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t) ||
      bytes % sizeof(wchar_t) != 0 || bytes > 32768 * sizeof(wchar_t)) {
    return {};
  }
  std::wstring value((bytes / sizeof(wchar_t)) + 1, L'\0');
  if (RegQueryValueExW(key, name, nullptr, &type,
                       reinterpret_cast<BYTE*>(value.data()), &bytes) !=
      ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) ||
      bytes % sizeof(wchar_t) != 0) {
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
         type == REG_DWORD && bytes == sizeof(DWORD);
}

bool SamePath(const std::wstring& a, const std::wstring& b) {
  return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(),
                              static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

std::wstring CanonicalPath(const std::wstring& path) {
  const DWORD required = ExpandEnvironmentStringsW(path.c_str(), nullptr, 0);
  if (required == 0 || required > 32768) {
    return path;
  }
  std::wstring expanded(required, L'\0');
  if (ExpandEnvironmentStringsW(path.c_str(), expanded.data(), required) == 0) {
    return path;
  }
  expanded.resize(wcslen(expanded.c_str()));
  if (expanded.size() > 38 && expanded[0] == L'{' && expanded[38] == L'\\') {
    GUID folder = {};
    if (SUCCEEDED(CLSIDFromString(expanded.substr(0, 38).c_str(), &folder))) {
      PWSTR raw = nullptr;
      if (SUCCEEDED(SHGetKnownFolderPath(folder, KF_FLAG_DONT_VERIFY, nullptr, &raw))) {
        const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned(raw, CoTaskMemFree);
        expanded = std::wstring(owned.get()) + expanded.substr(38);
      }
    }
  }
  if (expanded.starts_with(L"\\\\?\\")) {
    expanded.erase(0, 4);
  }
  std::ranges::replace(expanded, L'/', L'\\');
  return expanded;
}

std::vector<std::wstring> ReadOrder(HKEY key) {
  DWORD type = 0;
  DWORD bytes = 0;
  if (RegQueryValueExW(key, L"UIOrderList", nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
      type != REG_BINARY || bytes == 0 || bytes > 1024 * 1024 || bytes % sizeof(uint64_t) != 0) {
    return {};
  }
  std::vector<BYTE> data(bytes);
  if (RegQueryValueExW(key, L"UIOrderList", nullptr, &type, data.data(), &bytes) != ERROR_SUCCESS ||
      type != REG_BINARY || bytes % sizeof(uint64_t) != 0) {
    return {};
  }
  std::vector<std::wstring> order;
  for (size_t offset = 0; offset < bytes; offset += sizeof(uint64_t)) {
    uint64_t id = 0;
    std::memcpy(&id, data.data() + offset, sizeof(id));
    order.push_back(std::to_wstring(id));
  }
  return order;
}

}  // namespace

std::vector<NotifyIconSetting> ReadNotifyIconSettings(HKEY settings) {
  std::vector<NotifyIconSetting> out;
  if (settings == nullptr) {
    return out;
  }
  const auto order = ReadOrder(settings);
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
    setting.exePath = CanonicalPath(ReadString(entry, kValueExePath));
    DWORD uid = 0;
    setting.hasUid = ReadDword(entry, kValueUid, &uid);
    setting.uid = uid;
    DWORD promoted = 0;
    setting.isPromoted = ReadDword(entry, kValuePromoted, &promoted) && promoted != 0;
    const auto guid = ReadString(entry, L"IconGuid");
    setting.hasGuid = !guid.empty() && SUCCEEDED(CLSIDFromString(guid.c_str(), &setting.guid));
    const auto position = std::ranges::find(order, name);
    if (position != order.end()) {
      setting.order = static_cast<UINT>(position - order.begin());
    }
    RegCloseKey(entry);
    if (!setting.exePath.empty()) {
      out.push_back(std::move(setting));
    }
  }
  std::ranges::stable_sort(out, {}, &NotifyIconSetting::order);
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
  if (setting.hasGuid || icon.key.kind == IconKey::Kind::kGuid) {
    return setting.hasGuid && icon.key.kind == IconKey::Kind::kGuid &&
           IsEqualGUID(setting.guid, icon.key.guid) != FALSE;
  }
  if (icon.exePath.empty() || setting.exePath.empty()) {
    return false;
  }
  if (!SamePath(CanonicalPath(setting.exePath), CanonicalPath(icon.exePath))) {
    return false;
  }
  return setting.hasUid && setting.uid == icon.key.uid;
}

bool ApplyPromotion(TrayModel* model,
                    const std::vector<NotifyIconSetting>& settings) {
  if (model == nullptr) {
    return false;
  }
  bool changed = false;
  for (const TrayIcon& icon : model->icons()) {
    bool promoted = false;  // unknown icons remain reachable in overflow
    UINT order = UINT_MAX;
    const NotifyIconSetting* selected = nullptr;
    for (const NotifyIconSetting& setting : settings) {
      if (MatchesIcon(setting, icon)) {
        // Duplicate registrations can match one live icon. The entry the
        // shell places in UIOrderList is the current one; the earliest
        // position wins, and an unplaced entry never outranks a placed
        // one. Without any placement the first enumerated entry wins,
        // which keeps repeated reads deterministic.
        const auto better = [](const NotifyIconSetting& candidate,
                               const NotifyIconSetting& current) {
          const bool candidatePlaced = candidate.order != UINT_MAX;
          const bool currentPlaced = current.order != UINT_MAX;
          if (candidatePlaced != currentPlaced) {
            return candidatePlaced;
          }
          return candidatePlaced && candidate.order < current.order;
        };
        if (selected == nullptr || better(setting, *selected)) {
          selected = &setting;
        }
      }
    }
    if (selected != nullptr) {
      promoted = selected->isPromoted;
      order = selected->order;
    }
    if (model->SetPromotion(icon.key, promoted)) {
      changed = true;
    }
    changed = model->SetShellOrder(icon.key, order) || changed;
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
    if (RegNotifyChangeKeyValue(key, TRUE, REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_CHANGE_NAME, changed_,
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
      if (RegNotifyChangeKeyValue(key, TRUE, REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_CHANGE_NAME,
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
