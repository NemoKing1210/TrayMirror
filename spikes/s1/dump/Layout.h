#pragma once

// TRAYDATA/NOTIFYICONDATA layout (spec 3.3: establish by measurement only).
// The constants are filled in from the S2 spike results (plan, task 8); until
// then kLayoutKnown == false and decode mode is unavailable.
inline constexpr bool kLayoutKnown = false;
inline constexpr size_t kOffHwnd = 0;
inline constexpr size_t kOffUid = 0;
inline constexpr size_t kOffFlags = 0;
