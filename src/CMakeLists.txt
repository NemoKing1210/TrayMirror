# Spec 12.8: clang-tidy clean. Opt-in: when the tool is on PATH (VS
# "C++ Clang tools" component; build.ps1 wires the path), every
# translation unit under src/ is checked at compile time and a finding
# fails the build. The hook DLL is exempt (see src/hook/CMakeLists.txt).
find_program(TRAYMIRROR_CLANG_TIDY clang-tidy)
if(TRAYMIRROR_CLANG_TIDY)
  # CMake's clang-tidy launch does not forward the compiler's /std flag;
  # without it clang-tidy parses as C++14 and rejects C++20 constructs.
  # In clang-cl mode the pass-through spelling is /clang:-std=c++20.
  # NOTE: exceptions cannot be enabled through /clang: pass-through —
  # cl mode appends its default -fno-exceptions after all pass-through
  # args, so try/catch is a hard error under tidy. Source files checked
  # by tidy must not use try/catch (ledger ruling, task 8).
  set(CMAKE_CXX_CLANG_TIDY
      "${TRAYMIRROR_CLANG_TIDY};--extra-arg=/clang:-std=c++20")
endif()

# Spec 2.5 repository layout: proto (protocol library), hook (in-Explorer
# DLL), host (main process), tools (diagnostics). The hook and the host are
# x64-only: a 32-bit hook cannot be installed into a 64-bit Explorer.
add_subdirectory(proto)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  add_subdirectory(hook)
  add_subdirectory(host)
  add_subdirectory(tools)
endif()
