# Canaries: tiny programs with deliberate bugs. Each test PASSES only if the
# sanitizer reports the bug, proving the sanitizer preset is actually active.
# Built only when SASR_SANITIZER is set.

string(REPLACE "," ";" _sasr_sanitizers "${SASR_SANITIZER}")

function(sasr_add_canary name source expected_report)
    add_executable(${name} ${source})
    sasr_set_warnings(${name})
    target_link_libraries(${name} PRIVATE ${ARGN})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES
        PASS_REGULAR_EXPRESSION "${expected_report}"
        LABELS canary)
endfunction()

if("address" IN_LIST _sasr_sanitizers)
    sasr_add_canary(canary_asan asan_canary.cpp "heap-buffer-overflow")
endif()

if("undefined" IN_LIST _sasr_sanitizers)
    sasr_add_canary(canary_ubsan ubsan_canary.cpp "signed integer overflow")
endif()

if("thread" IN_LIST _sasr_sanitizers)
    find_package(Threads REQUIRED)
    sasr_add_canary(canary_tsan tsan_canary.cpp "data race" Threads::Threads)
endif()
