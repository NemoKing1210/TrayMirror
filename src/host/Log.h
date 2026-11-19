// Host logging (spec 12.7): console + rotating file under
// %LOCALAPPDATA%\TrayMirror\logs. Levels via --log-level; the release
// default is warn, the debug default info.
#pragma once

#include <memory>
#include <string>

#include <spdlog/logger.h>

// Initializes the host logger; returns the logger instance. Safe to call
// once at startup.
std::shared_ptr<spdlog::logger> InitHostLogging(const std::string& level);
