#include "Log.h"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <windows.h>

#include <filesystem>
#include <string>

std::shared_ptr<spdlog::logger> InitHostLogging(const std::string& level) {
  static std::shared_ptr<spdlog::logger> logger;
  if (logger != nullptr) {
    return logger;
  }

  std::filesystem::path dir;
  {
    wchar_t local[MAX_PATH] = {};
    size_t needed = 0;
    if (_wgetenv_s(&needed, local, MAX_PATH, L"LOCALAPPDATA") == 0 &&
        needed != 0 && local[0] != L'\0') {
      dir = std::filesystem::path(local) / "TrayMirror" / "logs";
    } else {
      dir = std::filesystem::temp_directory_path();
    }
  }
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);

  const auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
  std::shared_ptr<spdlog::sinks::sink> fileSink;
  {
    // No try/catch here: the clang-tidy launch cannot be given exceptions
    // (see the ruling in src/CMakeLists.txt). This spdlog build only has
    // the throwing filename constructor, so the file is probed first —
    // if the probe opens, the constructor's own open succeeds in
    // practice. The residual race (path becomes unwritable between the
    // two opens) is accepted: logging is best-effort, ledger task 8.
    // The vcpkg spdlog is built with narrow filenames, hence the narrow
    // path.
    const std::string path = (dir / "host.log").string();
    std::FILE* probe = nullptr;
    fopen_s(&probe, path.c_str(), "w");
    if (probe != nullptr) {
      fclose(probe);
      fileSink =
          std::make_shared<spdlog::sinks::basic_file_sink_mt>(path, true);
    } else {
      fileSink = nullptr;  // file logging is best-effort
    }
  }

  if (fileSink != nullptr) {
    logger = std::make_shared<spdlog::logger>(
        "traymirror", spdlog::sinks_init_list{console, fileSink});
  } else {
    logger = std::make_shared<spdlog::logger>("traymirror", console);
  }

#ifdef NDEBUG
  const std::string kDefaultLevel = "warn";  // spec 12.7
#else
  const std::string kDefaultLevel = "info";
#endif
  std::string lvl = level.empty() ? kDefaultLevel : level;
  spdlog::level::level_enum lvlEnum = spdlog::level::info;
  if (lvl == "trace") {
    lvlEnum = spdlog::level::trace;
  } else if (lvl == "debug") {
    lvlEnum = spdlog::level::debug;
  } else if (lvl == "warn") {
    lvlEnum = spdlog::level::warn;
  } else if (lvl == "err") {
    lvlEnum = spdlog::level::err;
  }
  logger->set_level(lvlEnum);
  spdlog::set_default_logger(logger);
  return logger;
}
