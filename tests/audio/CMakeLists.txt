sasr_add_test(sasr_audio_tests
    SOURCES wav_reader_test.cpp
    LIBS sasr::audio)
target_compile_definitions(sasr_audio_tests PRIVATE
    SASR_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/data")
