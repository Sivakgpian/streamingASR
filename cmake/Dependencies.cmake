include_guard(GLOBAL)

include(FetchContent)

# Dependencies are pinned by release tarball + SHA256 for reproducible builds.
# SYSTEM makes their headers system includes, so our warning flags never fire on them.

if(SASR_BUILD_TESTS)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK ON CACHE BOOL "" FORCE)
    FetchContent_Declare(googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
        URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
        SYSTEM)
    FetchContent_MakeAvailable(googletest)

    # Clang >= 21 warns inside gtest's own sources (gtest 1.17 predates the
    # warning). Silence it on gtest targets only; our code keeps it.
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag(-Wcharacter-conversion SASR_HAS_WCHARACTER_CONVERSION)
    if(SASR_HAS_WCHARACTER_CONVERSION)
        foreach(t IN ITEMS gtest gtest_main gmock gmock_main)
            target_compile_options(${t} PRIVATE -Wno-character-conversion)
        endforeach()
    endif()
endif()

if(SASR_BUILD_BENCHMARKS)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(benchmark
        URL https://github.com/google/benchmark/archive/refs/tags/v1.9.4.tar.gz
        URL_HASH SHA256=b334658edd35efcf06a99d9be21e4e93e092bd5f95074c1673d5c8705d95c104
        SYSTEM)
    FetchContent_MakeAvailable(benchmark)
endif()
