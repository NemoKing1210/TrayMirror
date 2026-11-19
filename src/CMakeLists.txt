# Spec 2.5 repository layout: proto (protocol library), hook (in-Explorer
# DLL), host (main process), tools (diagnostics). The hook and the host are
# x64-only: a 32-bit hook cannot be installed into a 64-bit Explorer.
add_subdirectory(proto)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  add_subdirectory(hook)
  add_subdirectory(host)
  add_subdirectory(tools)
endif()
