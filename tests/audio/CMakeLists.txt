sasr_add_test(sasr_audio_tests
    SOURCES wav_reader_test.cpp
    LIBS sasr::audio)
target_compile_definitions(sasr_audio_tests PRIVATE
    SASR_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/data")

# Needs sole ownership of global operator new/delete; incompatible with
# ASan/TSan, which already replace them (see tests/support/alloc_counter.hpp).
if(SASR_SANITIZER STREQUAL "")
    sasr_add_test(sasr_audio_alloc_tests
        SOURCES wav_reader_alloc_test.cpp
        LIBS sasr::audio sasr::test_alloc_counter)
endif()
