add_library(sasr_asr mock_backend.cpp)
add_library(sasr::asr ALIAS sasr_asr)

target_include_directories(sasr_asr PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(sasr_asr PUBLIC sasr::common)
sasr_set_warnings(sasr_asr)

if(SASR_WITH_WHISPER)
    target_sources(sasr_asr PRIVATE whisper_cpp_backend.cpp)
    target_link_libraries(sasr_asr PUBLIC whisper)
endif()
