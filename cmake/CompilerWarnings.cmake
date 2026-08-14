# Strict warnings for project code only; third-party targets never call this.
#
# Warnings are errors by default, which is what CI enforces, and the default
# configuration builds clean on the supported toolchains: the one known
# libstdc++ false positive (GCC 12/13 -Wstringop-overflow from inside
# <stl_algobase.h>) is suppressed at its single call site in
# src/quadrangulate/src/sparse_cholesky.cpp rather than tree-wide.
# CYBER_WARNINGS_AS_ERRORS=OFF stays as the escape hatch for an unforeseen
# toolchain diagnostic; leave it ON everywhere that gates a merge.
option(CYBER_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

function(cyber_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
        if(CYBER_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic
            -Wshadow -Wconversion -Wsign-conversion
            -Wnon-virtual-dtor -Wold-style-cast)
        if(CYBER_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()
