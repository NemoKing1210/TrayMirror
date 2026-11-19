# TrayMirrorHook — the only code that runs inside explorer.exe (spec 2.3).
# MODULE DLL; the production discipline (no CRT, kernel32-only, /MT) is
# applied by the hook task together with the real hook code.
#
# clang-tidy is deliberately OFF for this target (spec 12.4): kernel32-only,
# allocation-free, exception-free code with SEH boundaries follows a
# C-style discipline that modernize-/readability- checks would fight; its
# quality gate is review + the in-process selftest, not static analysis.
set_property(DIRECTORY PROPERTY CMAKE_CXX_CLANG_TIDY "")
add_library(TrayMirrorHook MODULE TrayMirrorHook.cpp)
set_target_properties(TrayMirrorHook PROPERTIES PREFIX "")
target_link_libraries(TrayMirrorHook PRIVATE traymirror_proto)
