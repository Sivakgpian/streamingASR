sasr_add_test(sasr_asr_tests
    SOURCES mock_backend_test.cpp
    LIBS sasr::asr)

if(SASR_WITH_WHISPER)
    sasr_add_test(sasr_whisper_backend_tests
        SOURCES whisper_cpp_backend_test.cpp
        LIBS sasr::asr sasr::audio sasr::streaming)
    target_compile_definitions(sasr_whisper_backend_tests PRIVATE
        SASR_TEST_WHISPER_MODEL="${PROJECT_SOURCE_DIR}/models/ggml-tiny.en.bin"
        SASR_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/data")
endif()
