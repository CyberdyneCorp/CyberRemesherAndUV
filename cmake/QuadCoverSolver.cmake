# In-process QuadCover seamless-UV solver (Milestone M4c).
#
# Builds the same Geogram 1.8.3 + AutoRemesher + isotropicremesher translation
# units that examples/reference/build_autoremesher.sh compiles (parsed from
# autoremesher.pro so the two stay in lockstep) PLUS examples/reference/
# autoremesher_solve.cpp into an ISOLATED static library `cyber_quadcover_solver`.
#
# This target must NOT inherit the main tree's strict C++20 warning flags: Geogram
# is C++14 and warns heavily. It is built at C++14 with -w, exactly mirroring the
# shell script's flags/includes/defines. cyber_set_warnings is never called on it.
function(cyber_add_quadcover_solver)
    set(_ref "${PROJECT_SOURCE_DIR}/examples/reference")
    set(_ar "${_ref}/autoremesher-src")

    # The AutoRemesher/Geogram sources are vendored on demand by the reference
    # build script (git clone). Fetch them here too so a clean checkout can
    # configure with -DCYBER_WITH_QUADCOVER=ON without a separate manual step.
    if(NOT EXISTS "${_ar}/autoremesher.pro")
        message(STATUS "cyber_quadcover_solver: cloning AutoRemesher sources into ${_ar}")
        execute_process(
            COMMAND git clone --depth 1 https://github.com/huxingyi/autoremesher.git "${_ar}"
            RESULT_VARIABLE _clone_rc)
        if(NOT _clone_rc EQUAL 0)
            message(FATAL_ERROR "cyber_quadcover_solver: failed to clone AutoRemesher sources")
        endif()
    endif()

    # Parse the SOURCES the .pro lists, keeping only the Qt-free core the harness
    # compiles (geogram subset + AutoRemesher + isotropicremesher). This is the
    # exact same filter as build_autoremesher.sh.
    file(STRINGS "${_ar}/autoremesher.pro" _pro_lines REGEX "^SOURCES \\+=")
    set(_sources "")
    foreach(_line IN LISTS _pro_lines)
        string(REGEX REPLACE "^SOURCES \\+=[ \t]+" "" _rel "${_line}")
        string(STRIP "${_rel}" _rel)
        if(_rel MATCHES "^(thirdparty/geogram|src/AutoRemesher|thirdparty/isotropicremesher)/")
            list(APPEND _sources "${_ar}/${_rel}")
        endif()
    endforeach()
    list(LENGTH _sources _n)
    message(STATUS "cyber_quadcover_solver: ${_n} vendored sources from autoremesher.pro")

    add_library(cyber_quadcover_solver STATIC
        ${_sources}
        "${_ref}/autoremesher_solve.cpp")

    # Mirror build_autoremesher.sh exactly: C++14, no warnings, static Geogram.
    # -std is applied via the CXX_STANDARD property so the ~11 vendored C files
    # keep their C standard (the script's CFLAGS carry no -std either).
    set_target_properties(cyber_quadcover_solver PROPERTIES
        CXX_STANDARD 14
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_compile_options(cyber_quadcover_solver PRIVATE -w -fopenmp -pthread)
    target_compile_definitions(cyber_quadcover_solver PRIVATE
        NDEBUG _USE_MATH_DEFINES NOMINMAX GEO_STATIC_LIBS)
    target_include_directories(cyber_quadcover_solver PRIVATE
        "${_ref}/qt-shim"
        "${_ar}/include"
        "${_ar}/thirdparty/eigen"
        "${_ar}/thirdparty/isotropicremesher"
        "${_ar}/thirdparty/geogram/geogram-1.8.3/src/lib"
        "${_ar}/thirdparty/geogram/geogram-1.8.3/src/lib/geogram/third_party/libMeshb/sources"
        "${_ar}/thirdparty/geogram")
    # The solve TU's public header lives next to it; expose it so consumers can
    # include "autoremesher_solve.hpp" without seeing any Geogram include path.
    target_include_directories(cyber_quadcover_solver PUBLIC "${_ref}")

    find_package(OpenMP REQUIRED)
    target_link_libraries(cyber_quadcover_solver PRIVATE OpenMP::OpenMP_CXX tbb z ${CMAKE_DL_LIBS} m)
endfunction()
