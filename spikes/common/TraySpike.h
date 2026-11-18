#pragma once
// Spike IPC object names (identical for TraySpikeCtl and HookDll).
// All names live in Local\ — the session namespace (see spec, section 3.1).

inline constexpr wchar_t kOffEventName[] = L"Local\\TrayMirrorSpike.Off";  // manual-reset, kill switch
inline constexpr wchar_t kStopEventName[] = L"Local\\TrayMirrorSpike.Stop";  // auto-reset, daemon signal
inline constexpr wchar_t kCtlMutexName[] = L"Local\\TrayMirrorSpike.Ctl";    // single instance of the daemon
inline constexpr wchar_t kPidFileName[] = L"TrayMirrorSpike.pid";           // in %TEMP%
inline constexpr wchar_t kSpikeLogFileName[] = L"TrayMirrorSpike.log";      // in %TEMP%
