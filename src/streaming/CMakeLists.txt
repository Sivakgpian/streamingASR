add_library(sasr_streaming session.cpp engine.cpp)
add_library(sasr::streaming ALIAS sasr_streaming)

target_include_directories(sasr_streaming PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(sasr_streaming PUBLIC sasr::common sasr::asr sasr::vad)
sasr_set_warnings(sasr_streaming)
