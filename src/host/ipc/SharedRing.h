// Host-side ownership of the shared ring: create the named mapping with
// a strict DACL (current user and LOCAL SYSTEM only, spec 3.1), publish
// the header, own the auto-reset signal event, and reopen read-only for
// the diagnostics tool. RAII: every successful create/open is closed by
// SharedRingClose.
#pragma once

#include "Ring.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>

struct SharedRingHandle {
  HANDLE map = nullptr;
  HANDLE signalEvent = nullptr;
  HANDLE view = nullptr;  // MapViewOfFile return (void* in win32)
  volatile RingHeader* header = nullptr;
  volatile uint8_t* ring = nullptr;
};

// Builds the SECURITY_ATTRIBUTES for the ring mapping: a DACL of the
// current user and LOCAL SYSTEM only (spec 3.1). Returns false with a
// message when the user SID or the SDDL conversion fails. Callers must
// treat a failure as a hard create error — a default-DACL fallback
// would silently broaden access.
bool BuildRingSecurityAttributes(SECURITY_ATTRIBUTES* sa, wchar_t* errBuf,
                                 size_t errCap);

// Creates `Local\TrayMirror.Ring.v<version>` (size: header + ring) with
// DACL S-1-5-18 (SYSTEM) + current user, and the auto-reset signal event
// `Local\TrayMirror.Signal`. A mapping that already exists is ADOPTED
// when it carries this build's magic/version/geometry (an orphaned ring
// after a host crash, the hook still holding its view: keep positions,
// refresh explorerPid — crash recovery); a foreign or hostile one is
// refused. Single instance is the mutex's job (spec 7.4), not the name.
bool SharedRingCreate(SharedRingHandle* out, DWORD explorerPid,
                      wchar_t* errBuf, size_t errCap);

// Opens the mapping read-only (diagnostics). Returns false when it does
// not exist.
bool SharedRingOpenRead(SharedRingHandle* out);

// Closes everything the handle owns. Safe on a zeroed handle.
void SharedRingClose(SharedRingHandle* h);
