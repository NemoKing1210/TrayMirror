find_package(GTest CONFIG REQUIRED)

add_executable(traymirror_unit test_proto.cpp test_ring.cpp test_sharedring.cpp
               test_traypayload.cpp test_hook.cpp test_host.cpp test_dump.cpp
               test_model.cpp test_initialsync.cpp test_monitors.cpp
               test_inbox.cpp test_iconcache.cpp test_hover.cpp)
# gtest's imported targets carry CXX_STANDARD 17; raising the consumer's
# standard avoids the "/std:c++17 redefined to /std:c++20" command-line
# warning (the repo norm is zero warnings).
set_target_properties(traymirror_unit PROPERTIES CXX_STANDARD 20)
target_link_libraries(traymirror_unit PRIVATE GTest::gtest_main traymirror_proto
                                             traymirror_host traymirror_dump_core)
target_compile_definitions(
  traymirror_unit PRIVATE
  TRAYMIRROR_HOOK_DLL_PATH=L"${CMAKE_BINARY_DIR}/src/hook/TrayMirrorHook.dll")
add_test(NAME traymirror_unit COMMAND traymirror_unit)
