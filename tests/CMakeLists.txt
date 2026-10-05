include(GoogleTest)

# sasr_add_test(<name> SOURCES <files...> LIBS <targets...>)
# Builds a GoogleTest executable and registers each TEST() as its own ctest case.
function(sasr_add_test name)
    cmake_parse_arguments(ARG "" "" "SOURCES;LIBS" ${ARGN})
    add_executable(${name} ${ARG_SOURCES})
    target_link_libraries(${name} PRIVATE ${ARG_LIBS} GTest::gtest_main GTest::gmock)
    sasr_set_warnings(${name})
    # PRE_TEST: discover at ctest time, not as a post-build step, so building
    # never runs (possibly sanitized) test binaries.
    gtest_discover_tests(${name} DISCOVERY_MODE PRE_TEST)
endfunction()

add_subdirectory(support)
add_subdirectory(common)
add_subdirectory(audio)
add_subdirectory(asr)
add_subdirectory(vad)
add_subdirectory(streaming)
add_subdirectory(profiling)

if(NOT SASR_SANITIZER STREQUAL "")
    add_subdirectory(sanitizer_canary)
endif()
