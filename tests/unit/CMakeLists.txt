find_package(GTest CONFIG REQUIRED)

add_executable(traymirror_unit test_proto.cpp test_ring.cpp test_sharedring.cpp)
target_link_libraries(traymirror_unit PRIVATE GTest::gtest_main traymirror_proto
                                             traymirror_host)
add_test(NAME traymirror_unit COMMAND traymirror_unit)
