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

// Creates `Local\TrayMirror.Ring.v<version>` (size: header + ring) with
// DACL S-1-5-18 (SYSTEM) + current user, and the auto-reset signal event
// `Local\TrayMirror.Signal`. Returns false (with a message in errBuf)
// when the mapping already exists — another live host owns it.
bool SharedRingCreate(SharedRingHandle* out, DWORD explorerPid,
                      wchar_t* errBuf, size_t errCap);

// Opens the mapping read-only (diagnostics). Returns false when it does
// not exist.
bool SharedRingOpenRead(SharedRingHandle* out);

// Closes everything the handle owns. Safe on a zeroed handle.
void SharedRingClose(SharedRingHandle* h);
