// Build identity for the acceptance checklist: the identifier embedded
// when this binary was configured and the full path it was loaded from.
#pragma once

#include <string>

namespace traymirror {

// Configure-time identifier (commit, configuration, configure timestamp).
// It is compiled into the binary, so a running process can prove which
// build it is.
const char* BuildId() noexcept;

// Full path of the running executable.
std::wstring ExecutablePath();

// One diagnostic line: "TrayMirror <build> (<path>)".
std::wstring VersionLine();

}  // namespace traymirror
