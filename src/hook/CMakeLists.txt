# TrayMirrorHook — the only code that runs inside explorer.exe (spec 2.3).
# MODULE DLL; the production discipline (no CRT, kernel32-only, /MT) is
# applied by the hook task together with the real hook code.
add_library(TrayMirrorHook MODULE TrayMirrorHook.cpp)
set_target_properties(TrayMirrorHook PROPERTIES PREFIX "")
target_link_libraries(TrayMirrorHook PRIVATE traymirror_proto)
