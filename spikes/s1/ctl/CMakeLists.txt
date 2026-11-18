# Spike controller (plan, task 4).
add_executable(TraySpikeCtl TraySpikeCtl.cpp)
target_link_libraries(TraySpikeCtl PRIVATE TrayCommon)

# The hook DLL must sit next to the EXE: LoadLibraryW by a relative path.
# POST_BUILD under Ninja does not fire when only the DLL changed and ctl was
# not rebuilt; therefore the copy is a separate rule depending on the DLL target.
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/TrayMirrorSpikeHook.dll
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:TrayMirrorSpikeHook>
          ${CMAKE_CURRENT_BINARY_DIR}/TrayMirrorSpikeHook.dll
  DEPENDS TrayMirrorSpikeHook)
add_custom_target(CtlHookDllCopy
  DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/TrayMirrorSpikeHook.dll)
add_dependencies(TraySpikeCtl CtlHookDllCopy)
