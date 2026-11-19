#include "BuildInfo.h"

#include <windows.h>

#include <cstring>

#ifndef TRAYMIRROR_BUILD_ID
#define TRAYMIRROR_BUILD_ID "unknown"
#endif

namespace traymirror {

const char* BuildId() noexcept {
  return TRAYMIRROR_BUILD_ID;
}

std::wstring ExecutablePath() {
  std::wstring buffer(32768, L'\0');
  const DWORD length = GetModuleFileNameW(
      nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (length == 0 || length >= buffer.size()) {
    return {};
  }
  buffer.resize(length);
  return buffer;
}

std::wstring VersionLine() {
  std::wstring line = L"TrayMirror ";
  const char* const id = BuildId();
  line.append(id, id + std::strlen(id));
  line += L" (";
  line += ExecutablePath();
  line += L")";
  return line;
}

}  // namespace traymirror
