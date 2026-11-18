# Spike hook DLL (plan, task 3).
# /MT — static CRT: the DLL must be self-contained inside explorer.exe
# (the dynamic debug CRT is not loaded in explorer).
add_library(TrayMirrorSpikeHook SHARED TrayHook.cpp)
target_link_libraries(TrayMirrorSpikeHook PRIVATE TrayCommon)
set_target_properties(TrayMirrorSpikeHook PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded")
