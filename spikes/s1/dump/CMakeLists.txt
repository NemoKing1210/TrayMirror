# Spike log dump/decoder (plan, task 7).
add_library(TrayDump STATIC Parse.cpp)
target_include_directories(TrayDump PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(TrayDump PUBLIC TrayCommon)

add_executable(TraySpikeDump TraySpikeDump.cpp)
target_link_libraries(TraySpikeDump PRIVATE TrayDump)

add_executable(DumpTests test/test_dump.cpp)
target_link_libraries(DumpTests PRIVATE TrayDump)
add_test(NAME DumpTests COMMAND DumpTests)
