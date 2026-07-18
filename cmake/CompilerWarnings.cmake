# Strict warnings for project code only; third-party targets never call this.
function(cyber_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /permissive-)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Werror
            -Wshadow -Wconversion -Wsign-conversion
            -Wnon-virtual-dtor -Wold-style-cast)
    endif()
endfunction()
