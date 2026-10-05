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

# Off by default: a from-scratch build is sizeable (ggml + whisper.cpp),
# and the sanitizer presets instrument it too (sanitizers are applied
# globally -- see Sanitizers.cmake -- which is correct but makes an
# ASan/TSan build of ggml's numeric kernels markedly slower). Turn on
# with -DSASR_WITH_WHISPER=ON.
option(SASR_WITH_WHISPER "Build the whisper.cpp ASR backend (src/asr/whisper_cpp_backend.*)" OFF)
if(SASR_WITH_WHISPER)
    set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(whisper_cpp
        URL https://github.com/ggml-org/whisper.cpp/archive/refs/tags/v1.9.4.tar.gz
        URL_HASH SHA256=57e280cee375ab02425b806ad5146b99f6eb9357e3c2b31357c8a6af2e2e44ae
        SYSTEM)
    FetchContent_MakeAvailable(whisper_cpp)
endif()
