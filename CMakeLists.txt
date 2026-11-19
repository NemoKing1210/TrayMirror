cmake_minimum_required(VERSION 3.20)

project(TrayMirror CXX)

if(NOT MSVC)
  message(FATAL_ERROR "TrayMirror: MSVC-only build (spec, section 2.5)")
endif()

# Spec 12.8: /W4 /WX; C++20 (section 0). A warning is a build failure.
# Sources are UTF-8: without /utf-8 MSVC decodes non-ASCII literals with
# the system ANSI code page, which silently corrupts measured shell text
# (for example a captured Russian tooltip) on a non-English machine.
add_compile_options(
  $<$<COMPILE_LANGUAGE:CXX>:/W4>
  $<$<COMPILE_LANGUAGE:CXX>:/WX>
  $<$<COMPILE_LANGUAGE:CXX>:/std:c++20>
  $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)

option(TRAYMIRROR_BUILD_SPK "Build the stage 0 spike tools" ON)
option(TRAYMIRROR_BUILD_TESTS "Build unit and fuzz tests" ON)

enable_testing()

# Every executable embeds the application manifest (comctl32 v6 +
# per-monitor DPI v2): without comctl32 v6 the process runs in the v5
# compatibility mode, where TTM_ADDTOOL rejects the SDK TOOLINFO and the
# tooltip never appears (measured on build 26300).
#
# The manifest replaces the linker-generated one after linking: with
# Ninja CMake always lets link.exe emit its own manifest resource, so
# /MANIFESTINPUT is rejected (LNK1220) and adding a second RT_MANIFEST
# duplicates the resource (CVT1100). mt.exe -outputresource overwrites
# resource 1 in place.
set(TRAYMIRROR_MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/resources/TrayMirror.manifest)
function(traymirror_add_manifest target)
  if(NOT CMAKE_MT)
    message(FATAL_ERROR "TrayMirror: mt.exe is required to embed the manifest")
  endif()
  add_custom_command(
    TARGET ${target}
    POST_BUILD
    COMMAND "${CMAKE_MT}" -nologo -manifest "${TRAYMIRROR_MANIFEST}"
            "-outputresource:$<TARGET_FILE:${target}>;#1"
    COMMENT "Embedding the TrayMirror application manifest")
endfunction()

add_subdirectory(src)

if(TRAYMIRROR_BUILD_SPK)
  add_subdirectory(spikes)
endif()

if(TRAYMIRROR_BUILD_TESTS)
  add_subdirectory(tests)
endif()
