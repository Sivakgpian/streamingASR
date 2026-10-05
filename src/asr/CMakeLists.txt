add_library(sasr_asr mock_backend.cpp)
add_library(sasr::asr ALIAS sasr_asr)

target_include_directories(sasr_asr PUBLIC "${PROJECT_SOURCE_DIR}/src")
sasr_set_warnings(sasr_asr)
