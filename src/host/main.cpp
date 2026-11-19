// TrayMirror host process entry point (spec section 2.4).
#include "HostApp.h"

#include <cstdio>

int wmain(int argc, wchar_t** argv) {
  HostAppConfig config;
  if (!HostAppParseArgs(argv + 1, argc - 1, &config)) {
    std::printf(
        "usage: TrayMirror [--selftest | --disable-hook | --diagnostic]\n"
        "                   [--duration <sec>] [--log-level <trace|debug|"
        "info|warn|err>]\n"
        "  no arguments: run until Ctrl+C / console close\n"
        "  --selftest: exercise the hook DLL in THIS process (no Explorer)\n"
        "  --disable-hook: arm the kill switch and exit\n");
    return 1;
  }
  return HostAppRun(config);
}
