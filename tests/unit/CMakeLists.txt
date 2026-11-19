find_package(GTest CONFIG REQUIRED)

add_executable(traymirror_unit test_proto.cpp test_ring.cpp test_sharedring.cpp
               test_traypayload.cpp test_hook.cpp)
target_link_libraries(traymirror_unit PRIVATE GTest::gtest_main traymirror_proto
                                             traymirror_host)
target_compile_definitions(
  traymirror_unit PRIVATE
  TRAYMIRROR_HOOK_DLL_PATH=L"${CMAKE_BINARY_DIR}/src/hook/TrayMirrorHook.dll")
add_test(NAME traymirror_unit COMMAND traymirror_unit)
