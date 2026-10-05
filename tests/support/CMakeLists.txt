# Opt-in support code for tests only (never linked into production targets).
add_library(sasr_test_alloc_counter alloc_counter.cpp)
add_library(sasr::test_alloc_counter ALIAS sasr_test_alloc_counter)
target_include_directories(sasr_test_alloc_counter PUBLIC "${PROJECT_SOURCE_DIR}/tests")
sasr_set_warnings(sasr_test_alloc_counter)
