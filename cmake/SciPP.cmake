# Vendored NumPP + SciPP for the min-cost-flow integer quadrangulator
# (docs/mcf-integer-layout-plan.md, M1).
#
# SciPP (scipp::sparse::csgraph::maximum_flow etc.) is the dependency-free
# replacement for Boost/LEMON used to port QuadriFlow's integer-layout stage. It
# is a compiled CMake library that resolves NumPP via find_package(NumPP CONFIG),
# so — mirroring cmake/QuadCoverSolver.cmake's clone-on-demand approach — this
# clones both repos, builds+installs NumPP then SciPP into a build-tree prefix at
# configure time (cached: only rebuilt if the install is missing), and imports the
# scipp::scipp target. Kept behind CYBER_WITH_SCIPP so normal builds stay
# dependency-free.
function(cyber_add_scipp)
    set(_vendor "${PROJECT_SOURCE_DIR}/examples/reference/scipp-src")
    set(_prefix "${CMAKE_BINARY_DIR}/scipp-install")
    set(_numpp "${_vendor}/NumPP")
    set(_scipp "${_vendor}/SciPP")

    # 1. Clone on demand (shallow), like the AutoRemesher/QuadCover vendoring.
    if(NOT EXISTS "${_numpp}/CMakeLists.txt")
        message(STATUS "cyber_scipp: cloning NumPP into ${_numpp}")
        execute_process(
            COMMAND git clone --depth 1 https://github.com/CyberdyneCorp/NumPP "${_numpp}"
            RESULT_VARIABLE _rc)
        if(NOT _rc EQUAL 0)
            message(FATAL_ERROR "cyber_scipp: failed to clone NumPP")
        endif()
    endif()
    if(NOT EXISTS "${_scipp}/CMakeLists.txt")
        message(STATUS "cyber_scipp: cloning SciPP into ${_scipp}")
        execute_process(
            COMMAND git clone --depth 1 https://github.com/CyberdyneCorp/SciPP "${_scipp}"
            RESULT_VARIABLE _rc)
        if(NOT _rc EQUAL 0)
            message(FATAL_ERROR "cyber_scipp: failed to clone SciPP")
        endif()
    endif()

    # 2. Build + install both at configure time (only if not already staged).
    #    NumPP first (SciPP's find_package(NumPP) resolves against this prefix).
    if(NOT EXISTS "${_prefix}/lib/cmake/SciPP/SciPPConfig.cmake")
        include(ProcessorCount)
        ProcessorCount(_jobs)
        if(_jobs EQUAL 0)
            set(_jobs 2)
        endif()
        message(STATUS "cyber_scipp: building NumPP + SciPP (first configure, ~minutes)")

        _cyber_scipp_stage("${_numpp}" "${_prefix}" "${_jobs}"
            "-DNUMPP_BUILD_TESTS=OFF;-DNUMPP_BUILD_SHARED=ON" "NumPP")
        _cyber_scipp_stage("${_scipp}" "${_prefix}" "${_jobs}"
            "-DSCIPP_BUILD_TESTS=OFF;-DSCIPP_NUMPP_PREFIX=${_prefix}" "SciPP")
    endif()

    # 3. Import scipp::scipp from the staged prefix. SciPPConfig does
    #    find_dependency(NumPP), so make NumPP resolvable there too (NumPP_DIR +
    #    the prefix on CMAKE_PREFIX_PATH); function scope keeps this local.
    set(NumPP_DIR "${_prefix}/lib/cmake/NumPP")
    list(APPEND CMAKE_PREFIX_PATH "${_prefix}")
    find_package(SciPP CONFIG REQUIRED)
    message(STATUS "cyber_scipp: imported SciPP ${SciPP_VERSION} (prefix ${_prefix})")
endfunction()

# Configure -> build -> install one vendored CMake project into `prefix`.
function(_cyber_scipp_stage src prefix jobs extra_args name)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -S "${src}" -B "${src}/build"
                -DCMAKE_BUILD_TYPE=Release "-DCMAKE_INSTALL_PREFIX=${prefix}" ${extra_args}
        RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "cyber_scipp: failed to configure ${name}")
    endif()
    execute_process(
        COMMAND ${CMAKE_COMMAND} --build "${src}/build" --parallel ${jobs}
        RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "cyber_scipp: failed to build ${name}")
    endif()
    execute_process(
        COMMAND ${CMAKE_COMMAND} --install "${src}/build"
        RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "cyber_scipp: failed to install ${name}")
    endif()
endfunction()
