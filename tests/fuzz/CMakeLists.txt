# Deterministic fuzz corpus for the host record parsing path (plan
# task 10): ValidateRingRecord / FindNextRecord / RingReaderGet /
# DecodeTrayPayload over xorshift64 garbage. Plain executable under
# ctest — the harness prints its own invariant summary.
add_executable(traymirror_fuzz RecordFuzz.cpp)
target_link_libraries(traymirror_fuzz PRIVATE traymirror_proto)
set_target_properties(traymirror_fuzz PROPERTIES CXX_STANDARD 20)
add_test(NAME traymirror_fuzz COMMAND traymirror_fuzz)
