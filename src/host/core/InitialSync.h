// Initial synchronization (spec 4.3): after the hook is installed and the
// reader is ready, the host broadcasts the TaskbarCreated message so every
// application re-registers its icons with NIM_ADD. Measured safe on a
// live tray in spike S3: Explorer's own dedup rejects duplicates, apps
// re-assert NIM_SETVERSION, no balloons repeat. Resync is requested at
// startup, on ring overflow, and on the manual command
// (`TrayMirror.exe --resync`).
#pragma once

#include <windows.h>

#include <functional>

namespace traymirror {

// Host-side only (never reaches the hook).
inline constexpr wchar_t kResyncEventName[] = L"Local\\TrayMirror.Resync";

using BroadcastFn = std::function<void()>;

// Returns the broadcast routine: RegisterWindowMessageW(TaskbarCreated)
// once, then SendNotifyMessageW(HWND_BROADCAST, ...) — non-blocking, so a
// hung third-party window cannot delay the caller (spec 4.3).
BroadcastFn DefaultTaskbarCreatedBroadcast();

// Signals a running host to resync. False when no host is running.
bool SignalResync();

}  // namespace traymirror
