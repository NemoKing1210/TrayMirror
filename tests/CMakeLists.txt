# Test targets are added by the tasks that first provide their sources:
# tests/unit (GoogleTest) with the proto task, tests/fuzz with the
# parser fuzz task. Tests run on x64 only: the x86 build exists solely
# for the WOW64 emulator spike.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  add_subdirectory(unit)
endif()
