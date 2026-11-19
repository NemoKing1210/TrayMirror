add_executable(traymirror_hook_latch test_hook_latch.cpp)
target_link_libraries(traymirror_hook_latch PRIVATE traymirror_host)
target_compile_definitions(
  traymirror_hook_latch PRIVATE
  TRAYMIRROR_HOOK_DLL_PATH=L"${CMAKE_BINARY_DIR}/src/hook/TrayMirrorHook.dll")
add_test(NAME traymirror_hook_latch COMMAND traymirror_hook_latch)

add_executable(traymirror_selftest test_selftest.cpp)
target_link_libraries(traymirror_selftest PRIVATE traymirror_host)
add_test(NAME traymirror_selftest COMMAND traymirror_selftest)
