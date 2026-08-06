# Strict warnings for project code only; third-party targets never call this.
#
# Warnings are errors by default, which is what CI enforces. Toolchains newer
# than CI's do emit diagnostics of their own -- GCC 13 raises a known
# -Wstringop-overflow false positive from inside libstdc++'s <stl_algobase.h>,
# for instance -- and there is no way to build the tree locally to look at them.
# CYBER_WARNINGS_AS_ERRORS=OFF downgrades them to warnings for that case. Leave
# it ON everywhere that gates a merge.
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
