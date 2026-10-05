add_library(sasr_vad energy_vad.cpp)
add_library(sasr::vad ALIAS sasr_vad)

target_include_directories(sasr_vad PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(sasr_vad PUBLIC sasr::common)
sasr_set_warnings(sasr_vad)
