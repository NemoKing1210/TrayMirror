# Shared spike structures (plan, task 2).
add_library(TrayCommon STATIC Record.cpp)
target_include_directories(TrayCommon PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(TrayCommonTests test/test_record.cpp)
target_link_libraries(TrayCommonTests PRIVATE TrayCommon)
add_test(NAME TrayCommonTests COMMAND TrayCommonTests)
