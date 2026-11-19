add_executable(traymirror_hook_latch test_hook_latch.cpp)
target_link_libraries(traymirror_hook_latch PRIVATE traymirror_host)
target_compile_definitions(
  traymirror_hook_latch PRIVATE
  TRAYMIRROR_HOOK_DLL_PATH=L"${CMAKE_BINARY_DIR}/src/hook/TrayMirrorHook.dll")
add_test(NAME traymirror_hook_latch COMMAND traymirror_hook_latch)

add_executable(traymirror_selftest test_selftest.cpp)
target_link_libraries(traymirror_selftest PRIVATE traymirror_host)
add_test(NAME traymirror_selftest COMMAND traymirror_selftest)

find_package(GTest CONFIG REQUIRED)

add_executable(traymirror_integration test_overlay.cpp test_flyout.cpp
                                 test_dead_icons.cpp)
set_target_properties(traymirror_integration PROPERTIES CXX_STANDARD 20)
target_link_libraries(traymirror_integration PRIVATE traymirror_host
                                                    GTest::gtest_main)
traymirror_add_manifest(traymirror_integration)
add_test(NAME traymirror_integration COMMAND traymirror_integration)
