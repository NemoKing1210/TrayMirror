#include "core/TrayModel.h"

#include <cstring>
#include <utility>

namespace traymirror {
namespace {

// NIM_* op codes and NIF_* bits as measured in spike S2.
constexpr uint32_t kNimAdd = 0;
constexpr uint32_t kNimModify = 1;
constexpr uint32_t kNimDelete = 2;
constexpr uint32_t kNimSetFocus = 3;
constexpr uint32_t kNimSetVersion = 4;

constexpr uint32_t kNifMessage = 0x1;
constexpr uint32_t kNifIcon = 0x2;
constexpr uint32_t kNifTip = 0x4;
constexpr uint32_t kNifState = 0x8;

constexpr size_t kNoIndex = static_cast<size_t>(-1);

// RAII around a kernel handle for the exe-path lookup.
class ScopedHandle {
 public:
  explicit ScopedHandle(HANDLE h) : h_(h) {}
  ~ScopedHandle() { CloseHandle(h_); }
  ScopedHandle(const ScopedHandle&) = delete;
  ScopedHandle& operator=(const ScopedHandle&) = delete;
  [[nodiscard]] HANDLE get() const noexcept { return h_; }

 private:
  HANDLE h_;
};

}  // namespace

bool IconKey::operator==(const IconKey& o) const noexcept {
  if (kind != o.kind) {
    return false;
  }
  if (kind == Kind::kGuid) {
    return std::memcmp(&guid, &o.guid, sizeof(guid)) == 0;
  }
  return hwnd == o.hwnd && uid == o.uid;
}

IconOps DefaultIconOps() {
  IconOps ops;
  ops.copy = CopyIcon;
  ops.destroy = DestroyIcon;
  return ops;
}

bool DefaultPidResolver(HWND hwnd, DWORD* pid, DWORD* tid) {
  if (hwnd == nullptr || pid == nullptr || tid == nullptr) {
    return false;
  }
  DWORD processId = 0;
  const DWORD threadId = GetWindowThreadProcessId(hwnd, &processId);
  if (threadId == 0 || processId == 0) {
    return false;
  }
  *pid = processId;
  *tid = threadId;
  return true;
}

std::wstring DefaultExePathResolver(DWORD pid) {
  if (pid == 0) {
    return {};
  }
  ScopedHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
  if (process.get() == nullptr) {
    return {};
  }
  wchar_t buf[1024] = {};
  auto size = static_cast<DWORD>((sizeof(buf) / sizeof(buf[0])) - 1);
  if (QueryFullProcessImageNameW(process.get(), 0, buf, &size) == FALSE) {
    return {};
  }
  return {buf, size};
}

TrayModel::TrayModel(IconOps icons, PidResolver pid, ExePathResolver exe)
    : iconOps_(icons), pid_(std::move(pid)), exe_(std::move(exe)) {}

TrayModel::~TrayModel() {
  for (auto& icon : icons_) {
    DestroyIconCopy(&icon);
  }
}

bool TrayModel::Apply(const TrayMessage& msg) {
  IconKey key;
  if (msg.hasGuid) {
    key.kind = IconKey::Kind::kGuid;
    key.guid = msg.guid;
  } else {
    key.kind = IconKey::Kind::kHwndUid;
    key.hwnd = SignExtendHwnd(msg.hwnd);
    key.uid = msg.uid;
  }
  const size_t idx = IndexOf(key);
  bool changed = false;

  switch (msg.nim) {
    case kNimAdd: {
      size_t target = idx;
      if (target == kNoIndex) {
        TrayIcon created;
        created.key = key;
        icons_.push_back(std::move(created));
        target = icons_.size() - 1;
        changed = true;
      }
      ApplyAdd(&icons_[target], msg, &changed);
      return changed;
    }
    case kNimModify: {
      if (idx == kNoIndex) {
        return false;
      }
      ApplyNidFields(&icons_[idx], msg, &changed);
      return changed;
    }
    case kNimDelete: {
      if (idx == kNoIndex) {
        return false;
      }
      DestroyIconCopy(&icons_[idx]);
      icons_.erase(icons_.begin() + static_cast<std::ptrdiff_t>(idx));
      return true;
    }
    case kNimSetVersion: {
      if (idx == kNoIndex) {
        return false;
      }
      if (icons_[idx].version != msg.version) {
        icons_[idx].version = msg.version;
        return true;
      }
      return false;
    }
    case kNimSetFocus:
    default:
      return false;
  }
}

const TrayIcon* TrayModel::Find(const IconKey& key) const {
  const size_t idx = IndexOf(key);
  return idx == kNoIndex ? nullptr : &icons_[idx];
}

size_t TrayModel::IndexOf(const IconKey& key) const {
  for (size_t i = 0; i < icons_.size(); ++i) {
    if (icons_[i].key == key) {
      return i;
    }
  }
  return kNoIndex;
}

const TrayModel::OwnerInfo& TrayModel::ResolveOwner(HWND hwnd) {
  for (const auto& entry : owners_) {
    if (entry.first == hwnd) {
      return entry.second;
    }
  }
  OwnerInfo info;
  DWORD processId = 0;
  DWORD threadId = 0;
  if (pid_ && pid_(hwnd, &processId, &threadId)) {
    info.pid = processId;
    info.tid = threadId;
  }
  if (exe_ && info.pid != 0) {
    info.exePath = exe_(info.pid);
  }
  owners_.emplace_back(hwnd, std::move(info));
  return owners_.back().second;
}

void TrayModel::ApplyAdd(TrayIcon* icon, const TrayMessage& msg,
                         bool* changed) {
  const HWND owner = SignExtendHwnd(msg.hwnd);
  if (icon->ownerHwnd != owner || icon->ownerPid == 0) {
    const OwnerInfo& info = ResolveOwner(owner);
    icon->ownerHwnd = owner;
    icon->ownerPid = info.pid;
    icon->ownerTid = info.tid;
    icon->exePath = info.exePath;
    *changed = true;
  }
  ApplyNidFields(icon, msg, changed);
}

void TrayModel::ApplyNidFields(TrayIcon* icon, const TrayMessage& msg,
                               bool* changed) {
  if ((msg.flags & kNifMessage) != 0 &&
      icon->callbackMessage != msg.callbackMsg) {
    icon->callbackMessage = msg.callbackMsg;
    *changed = true;
  }
  if ((msg.flags & kNifIcon) != 0) {
    HICON copy = nullptr;
    if (iconOps_.copy != nullptr && msg.icon != 0) {
      copy = iconOps_.copy(SignExtendIcon(msg.icon));
    }
    DestroyIconCopy(icon);
    icon->iconCopy = copy;
    ++icon->iconSerial;  // the renderer keys its cache off this
    *changed = true;
  }
  if ((msg.flags & kNifTip) != 0 && icon->tip != msg.tip) {
    icon->tip = msg.tip;
    *changed = true;
  }
  if ((msg.flags & kNifState) != 0) {
    const DWORD merged =
        (icon->state & ~msg.stateMask) | (msg.state & msg.stateMask);
    if (icon->state != merged || icon->stateMask != msg.stateMask) {
      icon->state = merged;
      icon->stateMask = msg.stateMask;
      *changed = true;
    }
  }
}

void TrayModel::DestroyIconCopy(TrayIcon* icon) const {
  if (icon->iconCopy != nullptr) {
    if (iconOps_.destroy != nullptr) {
      iconOps_.destroy(icon->iconCopy);
    }
    icon->iconCopy = nullptr;
  }
}

}  // namespace traymirror
