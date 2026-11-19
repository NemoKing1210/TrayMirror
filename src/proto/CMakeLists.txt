add_library(traymirror_proto STATIC Proto.cpp Crc32.cpp RecordCodec.cpp
            RingWriter.cpp RingReader.cpp TrayPayload.cpp)
target_include_directories(traymirror_proto PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
