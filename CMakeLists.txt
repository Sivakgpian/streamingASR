cmake_minimum_required(VERSION 3.25)

project(streamingasr
    VERSION 0.1.0
    DESCRIPTION "CPU-first streaming ASR engine"
    LANGUAGES CXX)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
    message(FATAL_ERROR
        "In-source builds are not allowed. Use a preset, e.g. `cmake --preset clang-debug`.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SASR_BUILD_TESTS "Build unit tests" ON)
option(SASR_BUILD_BENCHMARKS "Build benchmarks" ON)
option(SASR_BUILD_EXAMPLES "Build examples" ON)
option(SASR_WARNINGS_AS_ERRORS "Treat warnings in project targets as errors" ON)
option(SASR_FRAME_POINTERS "Keep frame pointers (cheap; makes perf and sanitizer stacks usable)" ON)
set(SASR_SANITIZER "" CACHE STRING
    "Sanitizers for all code: empty, 'address,undefined', or 'thread'")

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(Warnings)
include(Sanitizers)

# Applied globally (before dependencies are fetched) on purpose: sanitizers and
# frame pointers must cover third-party code too, unlike warnings.
if(SASR_FRAME_POINTERS)
    add_compile_options(-fno-omit-frame-pointer)
endif()
sasr_enable_sanitizers("${SASR_SANITIZER}")

include(Dependencies)

add_subdirectory(src)

if(SASR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(SASR_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

if(SASR_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
