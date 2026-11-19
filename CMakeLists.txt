cmake_minimum_required(VERSION 3.20)

project(TrayMirror CXX)

if(NOT MSVC)
  message(FATAL_ERROR "TrayMirror: MSVC-only build (spec, section 2.5)")
endif()

# Spec 12.8: /W4 /WX; C++20 (section 0). A warning is a build failure.
add_compile_options(/W4 /WX /std:c++20)

option(TRAYMIRROR_BUILD_SPK "Build the stage 0 spike tools" ON)
option(TRAYMIRROR_BUILD_TESTS "Build unit and fuzz tests" ON)

enable_testing()

add_subdirectory(src)

if(TRAYMIRROR_BUILD_SPK)
  add_subdirectory(spikes)
endif()

if(TRAYMIRROR_BUILD_TESTS)
  add_subdirectory(tests)
endif()
