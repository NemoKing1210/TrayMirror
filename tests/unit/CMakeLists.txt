find_package(GTest CONFIG REQUIRED)

add_executable(traymirror_unit test_proto.cpp)
target_link_libraries(traymirror_unit PRIVATE GTest::gtest_main traymirror_proto)
add_test(NAME traymirror_unit COMMAND traymirror_unit)
