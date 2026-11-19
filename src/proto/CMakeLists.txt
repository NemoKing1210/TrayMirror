# Two protocol libraries (plan task 7):
# - traymirror_proto: the full set, host-side (STL/CRT allowed).
# - traymirror_proto_hook: the kernel32-safe subset linked into the hook
#   DLL — only files free of STL, CRT calls and C++ runtime dependencies
#   (spec 12.4). Files move here only after review against that rule.
add_library(traymirror_proto STATIC Proto.cpp Crc32.cpp RecordCodec.cpp
            RingWriter.cpp RingReader.cpp TrayPayload.cpp)
target_include_directories(traymirror_proto PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_library(traymirror_proto_hook STATIC Crc32.cpp RecordCodec.cpp
            RingWriter.cpp)
target_include_directories(traymirror_proto_hook PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
