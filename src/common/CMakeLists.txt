add_library(sasr_common)
add_library(sasr::common ALIAS sasr_common)

target_sources(sasr_common PRIVATE version.cpp config.cpp)
target_include_directories(sasr_common PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_compile_definitions(sasr_common PRIVATE
    SASR_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    SASR_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    SASR_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    SASR_VERSION_STRING="${PROJECT_VERSION}")
sasr_set_warnings(sasr_common)
