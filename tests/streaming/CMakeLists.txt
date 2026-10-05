sasr_add_test(sasr_streaming_tests
    SOURCES
        utterance_buffer_test.cpp
        preroll_ring_test.cpp
        session_test.cpp
        engine_test.cpp
    LIBS sasr::streaming sasr::asr)

# Needs sole ownership of global operator new/delete; incompatible with
# ASan/TSan, which already replace them (see tests/support/alloc_counter.hpp).
if(SASR_SANITIZER STREQUAL "")
    sasr_add_test(sasr_streaming_alloc_tests
        SOURCES utterance_buffer_alloc_test.cpp
        LIBS sasr::streaming sasr::test_alloc_counter)
endif()
