# TrayMirrorHook — the only code that runs inside explorer.exe (spec 2.3).
# MODULE DLL, static CRT (/MT): self-contained inside Explorer.
#
# clang-tidy is deliberately OFF for this target (spec 12.4):
# kernel32-only, allocation-free, exception-free code with SEH
# boundaries follows a C-style discipline that modernize-/readability-
# checks would fight; its quality gate is review + the in-process
# selftest, not static analysis.
set_property(DIRECTORY PROPERTY CMAKE_CXX_CLANG_TIDY "")
add_library(TrayMirrorHook MODULE TrayMirrorHook.cpp)
# The tidy exemption must land on the target: CXX_CLANG_TIDY is a target
# property initialized from the variable, and the directory property
# does not override it.
set_target_properties(TrayMirrorHook PROPERTIES
                      CXX_CLANG_TIDY ""
                      PREFIX ""
                      MSVC_RUNTIME_LIBRARY "MultiThreaded")
target_link_libraries(TrayMirrorHook PRIVATE traymirror_proto_hook)
