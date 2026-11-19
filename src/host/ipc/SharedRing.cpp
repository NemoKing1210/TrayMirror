#include "SharedRing.h"

#include "Proto.h"

#include <sddl.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

void SetErr(wchar_t* errBuf, size_t errCap, const wchar_t* what) {
  if (errBuf != nullptr && errCap != 0) {
    _snwprintf_s(errBuf, errCap, _TRUNCATE, L"%s (GetLastError=%lu)", what,
                GetLastError());
  }
}

std::wstring CurrentUserSid() {
  HANDLE token = nullptr;
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == 0) {
    return {};
  }
  DWORD size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<uint8_t> buf(size);
  wchar_t* sid = nullptr;
  std::wstring out;
  if (GetTokenInformation(token, TokenUser, buf.data(), size, &size) != 0) {
    const auto* user = reinterpret_cast<const TOKEN_USER*>(buf.data());
    if (ConvertSidToStringSidW(user->User.Sid, &sid) != 0 && sid != nullptr) {
      out = sid;
      LocalFree(sid);
    }
  }
  CloseHandle(token);
  return out;
}

}  // namespace

bool SharedRingCreate(SharedRingHandle* out, DWORD explorerPid,
                      wchar_t* errBuf, size_t errCap) {
  *out = {};

  // DACL: current user and LOCAL SYSTEM only (spec 3.1).
  const std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;" + CurrentUserSid() +
                            L")";
  SECURITY_ATTRIBUTES sa = {};
  sa.nLength = sizeof(sa);
  SECURITY_ATTRIBUTES* saPtr = nullptr;
  if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
          sddl.c_str(), SDDL_REVISION_1, &sa.lpSecurityDescriptor,
          nullptr) != 0) {
    saPtr = &sa;
  }

  const SIZE_T viewSize = sizeof(RingHeader) + kRingSizeBytes;
  HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, saPtr, PAGE_READWRITE,
                                  0, static_cast<DWORD>(viewSize), kRingName);
  if (saPtr != nullptr) {
    LocalFree(sa.lpSecurityDescriptor);
  }
  if (map == nullptr) {
    SetErr(errBuf, errCap, L"CreateFileMappingW failed");
    return false;
  }
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    // The name is taken. Two cases:
    //  - another live host owns it: the single-instance mutex (spec 7.4)
    //    is the guard against that, not this check;
    //  - an orphaned ring left by a crashed host while the hook still
    //    holds its view: adopt it and keep reading — crash recovery.
    // A foreign or stale ring (bad magic/version) is still refused.
    auto* view = static_cast<uint8_t*>(
        MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    if (view == nullptr) {
      SetErr(errBuf, errCap, L"MapViewOfFile(existing) failed");
      CloseHandle(map);
      return false;
    }
    const auto* header = reinterpret_cast<const RingHeader*>(view);
    // A same-name mapping with the right magic/version but a geometry
    // no host of this build would create is hostile or corrupt: the
    // hook trusts the published ringSize, so adopting a foreign one
    // would make it write out of bounds inside explorer.exe. Refuse.
    if (header->magic != kRingMagic || header->protoVersion != kProtoVersion ||
        header->headerSize != sizeof(RingHeader) ||
        header->ringSize != kRingSizeBytes) {
      SetErr(errBuf, errCap,
             L"existing ring mapping is not ours (magic/version/geometry)");
      UnmapViewOfFile(view);
      CloseHandle(map);
      return false;
    }
    // Adopted: keep positions/heartbeat, refresh explorerPid, raise the
    // host flag word to zero (a new host session starts non-diagnostic).
    auto* vol = reinterpret_cast<volatile RingHeader*>(view);
    vol->explorerPid = explorerPid;
    vol->flags = 0;

    HANDLE signal = CreateEventW(nullptr, FALSE, FALSE, kSignalEventName);
    if (signal == nullptr) {
      SetErr(errBuf, errCap, L"CreateEventW(signal) failed");
      UnmapViewOfFile(view);
      CloseHandle(map);
      return false;
    }
    out->map = map;
    out->signalEvent = signal;
    out->view = view;
    out->header = vol;
    out->ring = view + sizeof(RingHeader);
    return true;
  }

  auto* view = static_cast<uint8_t*>(
      MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, viewSize));
  if (view == nullptr) {
    SetErr(errBuf, errCap, L"MapViewOfFile failed");
    CloseHandle(map);
    return false;
  }

  HANDLE signal = CreateEventW(nullptr, FALSE, FALSE, kSignalEventName);
  if (signal == nullptr) {
    SetErr(errBuf, errCap, L"CreateEventW(signal) failed");
    UnmapViewOfFile(view);
    CloseHandle(map);
    return false;
  }

  auto* header = reinterpret_cast<volatile RingHeader*>(view);
  // Publish the header with magic last: an early reader sees zeros, not
  // a half-initialized structure.
  header->protoVersion = kProtoVersion;
  header->headerSize = sizeof(RingHeader);
  header->ringSize = kRingSizeBytes;
  header->writePos = 0;
  header->readPos = 0;
  header->hookAlive = 0;
  header->droppedCount = 0;
  header->explorerPid = explorerPid;
  header->flags = 0;
  header->magic = kRingMagic;

  out->map = map;
  out->signalEvent = signal;
  out->view = view;
  out->header = header;
  out->ring = view + sizeof(RingHeader);
  return true;
}

bool SharedRingOpenRead(SharedRingHandle* out) {
  *out = {};

  HANDLE map = OpenFileMappingW(FILE_MAP_READ, FALSE, kRingName);
  if (map == nullptr) {
    return false;
  }
  auto* view = static_cast<uint8_t*>(
      MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0));
  if (view == nullptr) {
    CloseHandle(map);
    return false;
  }
  out->map = map;
  out->view = view;
  out->header = reinterpret_cast<volatile RingHeader*>(view);
  out->ring = view + sizeof(RingHeader);
  return true;
}

void SharedRingClose(SharedRingHandle* h) {
  if (h->view != nullptr) {
    UnmapViewOfFile(h->view);
  }
  if (h->map != nullptr) {
    CloseHandle(h->map);
  }
  if (h->signalEvent != nullptr) {
    CloseHandle(h->signalEvent);
  }
  *h = {};
}
