// TrayModel — the icon model (spec 4.1/4.2): keyed by (hWnd, uID) or
// guidItem, fed with decoded TrayMessages, owning icon copies. Host-only
// code (STL allowed); the model lives on the UI thread.
#pragma once

#include <windows.h>

#include "TrayPayload.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace traymirror {

// Sign-extends a 32-bit handle from the payload (spec 4.2 WOW64): the
// inter-process layout stores handles as u32; a WOW64 sender's handle
// with the high bit set must map to the sign-extended 64-bit value.
// LongToHandle (basetsd.h) is the documented Win32 spelling of exactly
// this conversion.
inline HWND SignExtendHwnd(uint32_t v) {
  return reinterpret_cast<HWND>(LongToHandle(static_cast<LONG>(v)));
}

inline HICON SignExtendIcon(uint32_t v) {
  return reinterpret_cast<HICON>(LongToHandle(static_cast<LONG>(v)));
}

// The icon key (spec 4.1): (hWnd, uID), or guidItem when NIF_GUID is set.
struct IconKey {
  enum class Kind : uint8_t { kHwndUid, kGuid };
  Kind kind = Kind::kHwndUid;
  HWND hwnd = nullptr;
  UINT uid = 0;
  GUID guid = {};
  bool operator==(const IconKey& o) const noexcept;
};

struct TrayIcon {
  IconKey key;
  HWND ownerHwnd = nullptr;
  DWORD ownerPid = 0;
  DWORD ownerTid = 0;
  UINT callbackMessage = 0;
  HICON iconCopy = nullptr;  // owned (CopyIcon); destroyed on replace/remove
  UINT iconSerial = 0;       // bumps whenever iconCopy is replaced
  std::wstring tip;
  DWORD state = 0;      // NIS_* bits
  DWORD stateMask = 0;  // mask of the last NIF_STATE update
  UINT version = 0;     // 0 or 4 (NIM_SETVERSION)
  std::wstring exePath;
  // Stage 3 (spec 4.5): registry promotion decides strip versus overflow;
  // an icon with no matching registry entry stays promoted.
  bool promoted = true;
  // Tick of the last applied message (diagnostics and promotion matching).
  ULONGLONG lastSeen = 0;

  [[nodiscard]] bool hidden() const noexcept { return (state & 0x1) != 0; }  // NIS_HIDDEN
};

// Injectable icon ownership (real Win32 calls by default).
struct IconOps {
  HICON (*copy)(HICON) = nullptr;
  BOOL (*destroy)(HICON) = nullptr;
};
IconOps DefaultIconOps();

using PidResolver = std::function<bool(HWND, DWORD*, DWORD*)>;
using ExePathResolver = std::function<std::wstring(DWORD)>;

bool DefaultPidResolver(HWND hwnd, DWORD* pid, DWORD* tid);
std::wstring DefaultExePathResolver(DWORD pid);

class TrayModel {
 public:
  explicit TrayModel(IconOps icons = DefaultIconOps(),
                     PidResolver pid = DefaultPidResolver,
                     ExePathResolver exe = DefaultExePathResolver);
  ~TrayModel();
  TrayModel(const TrayModel&) = delete;
  TrayModel& operator=(const TrayModel&) = delete;

  // Applies one decoded record (spec 4.2). Returns true iff the visible
  // model changed, so callers can mark overlays dirty.
  bool Apply(const TrayMessage& msg);

  [[nodiscard]] const std::vector<TrayIcon>& icons() const noexcept {
    return icons_;
  }
  [[nodiscard]] const TrayIcon* Find(const IconKey& key) const;

  // Stage 3 (spec 4.5): applies a promotion decision from the registry.
  // Returns true iff the visible model changed (the icon moved between
  // the strip and the overflow).
  bool SetPromotion(const IconKey& key, bool promoted);

 private:
  struct OwnerInfo {
    DWORD pid = 0;
    DWORD tid = 0;
    std::wstring exePath;
  };

  [[nodiscard]] size_t IndexOf(const IconKey& key) const;
  const OwnerInfo& ResolveOwner(HWND hwnd);
  void ApplyAdd(TrayIcon* icon, const TrayMessage& msg, bool* changed);
  void ApplyNidFields(TrayIcon* icon, const TrayMessage& msg, bool* changed);
  void DestroyIconCopy(TrayIcon* icon) const;

  IconOps iconOps_;
  PidResolver pid_;
  ExePathResolver exe_;
  std::vector<TrayIcon> icons_;
  std::vector<std::pair<HWND, OwnerInfo>> owners_;
};

}  // namespace traymirror
