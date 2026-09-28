add_executable(sasr_version version.cpp)
target_link_libraries(sasr_version PRIVATE sasr::common)
sasr_set_warnings(sasr_version)
