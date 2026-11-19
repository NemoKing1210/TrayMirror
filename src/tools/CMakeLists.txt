# The snapshot parser is a separate static library so the unit suite can
# exercise it directly (plan task 9); the executable adds only the CLI
# and the read-only mapping open.
add_library(traymirror_dump_core STATIC DumpCore.cpp)
target_include_directories(traymirror_dump_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(traymirror_dump_core PUBLIC traymirror_proto)

add_executable(TrayMirrorDump TrayMirrorDump.cpp)
target_link_libraries(TrayMirrorDump PRIVATE traymirror_dump_core traymirror_host)
