cmake_minimum_required(VERSION 3.20)

project(TrayMirrorSpikes CXX)

if(NOT MSVC)
  message(FATAL_ERROR "TrayMirror: MSVC-only build (spec, section 2.5)")
endif()

# Spec 12.8: /W4 /WX; C++20 (section 0).
add_compile_options(/W4 /WX /std:c++20)

enable_testing()

add_subdirectory(spikes)
