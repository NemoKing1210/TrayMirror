# Host-side libraries (spec 2.4/2.5): shared-memory IPC, hook install,
# the reader thread, logging. The host is a normal user process — STL is
# allowed here, unlike the hook DLL.
find_package(spdlog CONFIG REQUIRED)
find_package(Threads REQUIRED)

# The acceptance checklist must prove which binary is running. The
# identifier is captured at configure time (scripts/build.ps1
# reconfigures on every build) and compiled into the host library.
execute_process(
  COMMAND git rev-parse --short=12 HEAD
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  OUTPUT_VARIABLE TRAYMIRROR_GIT_HASH
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)
if(NOT TRAYMIRROR_GIT_HASH)
  set(TRAYMIRROR_GIT_HASH "nogit")
endif()
execute_process(
  COMMAND git status --porcelain --untracked-files=no
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  OUTPUT_VARIABLE TRAYMIRROR_GIT_DIRTY
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)
if(TRAYMIRROR_GIT_DIRTY)
  string(APPEND TRAYMIRROR_GIT_HASH "-dirty")
endif()
# No timestamp: an identifier that changes on every configure would
# force a relink of every target on every build and defeat incremental
# builds. The acceptance checklist records the executable's own hash
# alongside this identifier.
set(TRAYMIRROR_BUILD_ID
    "${TRAYMIRROR_GIT_HASH}-${CMAKE_BUILD_TYPE}")

add_library(traymirror_host STATIC
            ipc/SharedRing.cpp ipc/SingleInstance.cpp
            HookInstall.cpp ReaderThread.cpp Log.cpp HostApp.cpp
            BuildInfo.cpp
            core/TrayModel.cpp core/InitialSync.cpp core/ModelInbox.cpp
            core/PromotionStore.cpp core/DeadIconWatcher.cpp
            dispatch/Dispatch.cpp
            win/Monitors.cpp
            render/Overlay.cpp render/OverlayThread.cpp
            render/CellLayout.cpp render/Flyout.cpp
            render/IconCache.cpp render/IconRasterizer.cpp render/Tooltip.cpp
            render/Theme.cpp render/NativePopupSurface.cpp
            render/PopupTransition.cpp)
target_include_directories(traymirror_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(traymirror_host PRIVATE
                           TRAYMIRROR_BUILD_ID="${TRAYMIRROR_BUILD_ID}")
target_link_libraries(traymirror_host
                      PUBLIC traymirror_proto
                      PRIVATE spdlog::spdlog Threads::Threads
                              uiautomationcore ole32 oleaut32
                              d2d1 dwrite dcomp d3d11 dxgi dwmapi shcore comctl32
                              advapi32 windowscodecs shell32 msimg32)

enable_language(RC)
set_source_files_properties(${PROJECT_SOURCE_DIR}/resources/TrayMirror.rc
  PROPERTIES OBJECT_DEPENDS ${PROJECT_SOURCE_DIR}/assets/branding/traymirror-icon.ico)
add_executable(TrayMirror main.cpp ${PROJECT_SOURCE_DIR}/resources/TrayMirror.rc)
target_include_directories(TrayMirror PRIVATE ${PROJECT_SOURCE_DIR}/assets/branding)
target_link_libraries(TrayMirror PRIVATE traymirror_host)
traymirror_add_manifest(TrayMirror)
