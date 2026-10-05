add_library(sasr_profiling metrics.cpp)
add_library(sasr::profiling ALIAS sasr_profiling)

target_include_directories(sasr_profiling PUBLIC "${PROJECT_SOURCE_DIR}/src")
sasr_set_warnings(sasr_profiling)
