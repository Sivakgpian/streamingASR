include_guard(GLOBAL)

# Enables sanitizers for every target defined after this call, including
# dependencies. TSan in particular reports false positives if only part of the
# program is instrumented, so this is deliberately global.
function(sasr_enable_sanitizers sanitizers)
    if(sanitizers STREQUAL "")
        return()
    endif()

    string(REPLACE "," ";" requested "${sanitizers}")
    foreach(s IN LISTS requested)
        if(NOT s MATCHES "^(address|undefined|thread|leak)$")
            message(FATAL_ERROR "Unknown sanitizer '${s}' in SASR_SANITIZER='${sanitizers}'")
        endif()
    endforeach()
    if("thread" IN_LIST requested AND ("address" IN_LIST requested OR "leak" IN_LIST requested))
        message(FATAL_ERROR "ThreadSanitizer cannot be combined with AddressSanitizer/LeakSanitizer")
    endif()

    set(flags -fsanitize=${sanitizers} -fno-omit-frame-pointer)
    if("undefined" IN_LIST requested)
        # Make UB a hard failure so tests actually fail instead of just logging.
        list(APPEND flags -fno-sanitize-recover=undefined)
    endif()

    add_compile_options(${flags})
    add_link_options(-fsanitize=${sanitizers})
    message(STATUS "Sanitizers enabled: ${sanitizers}")
endfunction()
