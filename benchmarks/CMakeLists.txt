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
