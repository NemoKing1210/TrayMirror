# Spike S1 (plan, tasks 3–9).
# The hook and the controller only make sense under x64: the target machine's
# explorer.exe is x64, and an x86 hook cannot be installed into a 64-bit Explorer.
# For the WOW64 spike (S2) only an x86 build of TrayIconEmu is needed (a 32-bit
# Shell_NotifyIcon sender).
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  add_subdirectory(hook)
  add_subdirectory(ctl)
endif()
add_subdirectory(emu)
add_subdirectory(dump)
