# Host-side libraries (spec 2.4/2.5): shared-memory IPC, hook install,
# the reader thread, logging. The host is a normal user process — STL is
# allowed here, unlike the hook DLL.
find_package(spdlog CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_library(traymirror_host STATIC
            ipc/SharedRing.cpp ipc/SingleInstance.cpp
            HookInstall.cpp ReaderThread.cpp Log.cpp HostApp.cpp
            core/TrayModel.cpp core/InitialSync.cpp core/ModelInbox.cpp
            win/Monitors.cpp
            render/Overlay.cpp render/OverlayThread.cpp)
target_include_directories(traymirror_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(traymirror_host
                      PUBLIC traymirror_proto
                      PRIVATE spdlog::spdlog Threads::Threads
                              uiautomationcore ole32 oleaut32
                              d2d1 dcomp d3d11 dxgi dwmapi shcore)

add_executable(TrayMirror main.cpp)
target_link_libraries(TrayMirror PRIVATE traymirror_host)
