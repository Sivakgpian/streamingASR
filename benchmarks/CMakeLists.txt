# sasr_add_benchmark(<name> SOURCES <files...> LIBS <targets...>)
function(sasr_add_benchmark name)
    cmake_parse_arguments(ARG "" "" "SOURCES;LIBS" ${ARGN})
    add_executable(${name} ${ARG_SOURCES})
    target_link_libraries(${name} PRIVATE ${ARG_LIBS} benchmark::benchmark_main)
    sasr_set_warnings(${name})
endfunction()

sasr_add_benchmark(sasr_common_bench
    SOURCES common/version_bench.cpp
    LIBS sasr::common)

sasr_add_benchmark(sasr_streaming_bench
    SOURCES streaming/streaming_bench.cpp
    LIBS sasr::streaming sasr::asr sasr::profiling)

sasr_add_benchmark(sasr_threaded_bench
    SOURCES streaming/threaded_bench.cpp
    LIBS sasr::streaming sasr::asr sasr::profiling)

if(SASR_WITH_WHISPER)
    sasr_add_benchmark(sasr_whisper_bench
        SOURCES streaming/whisper_bench.cpp
        LIBS sasr::streaming sasr::asr sasr::audio sasr::profiling)
    target_compile_definitions(sasr_whisper_bench PRIVATE
        SASR_BENCH_WHISPER_MODEL="${PROJECT_SOURCE_DIR}/models/ggml-tiny.en.bin"
        SASR_BENCH_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/data")
endif()
