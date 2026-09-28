include_guard(GLOBAL)

# Applies the project warning set to a single target. Never applied globally, so
# third-party code (GoogleTest, Benchmark, later ASR runtimes) is unaffected.
function(sasr_set_warnings target)
    set(common_warnings
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Wcast-align
        -Woverloaded-virtual
        -Wdouble-promotion
        -Wformat=2
        -Wimplicit-fallthrough
        -Wunused)

    set(gcc_warnings
        -Wmisleading-indentation
        -Wduplicated-cond
        -Wduplicated-branches
        -Wlogical-op)

    target_compile_options(${target} PRIVATE
        ${common_warnings}
        $<$<CXX_COMPILER_ID:GNU>:${gcc_warnings}>
        $<$<BOOL:${SASR_WARNINGS_AS_ERRORS}>:-Werror>)
endfunction()
