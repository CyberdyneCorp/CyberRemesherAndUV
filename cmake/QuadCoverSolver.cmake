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

# Turn the portable OpenMP/TBB fallback below into a configure error. Release and
# CI lanes that are meant to SHIP the vendored field set this, so the field solver
# in an artifact is chosen deliberately instead of inherited from the runner image.
option(CYBER_REQUIRE_QUADCOVER
       "Fail configuration if the in-process Geogram field cannot be built" OFF)

# Ensure the vendored AutoRemesher checkout at ${_ar} sits at the exact commit
# recorded in examples/reference/autoremesher.pin (shared with
# build_autoremesher.sh). Every benchmark number flows through this field
# solver, so an unpinned or stale checkout is an unpinned benchmark: the two
# revisions seen in the wild produce different, and in one case
# nondeterministic, solvers. Fetches the pinned commit into a fresh checkout,
# or force-resets a checkout found at any other commit.
function(cyber_fetch_pinned_autoremesher _ar _pin)
    set(_url "https://github.com/huxingyi/autoremesher.git")
    if(NOT EXISTS "${_ar}/.git")
        message(STATUS "cyber_quadcover_solver: fetching AutoRemesher ${_pin} into ${_ar}")
        execute_process(COMMAND git init -q "${_ar}" RESULT_VARIABLE _rc)
        if(_rc EQUAL 0)
            execute_process(COMMAND git -C "${_ar}" remote add origin "${_url}"
                            RESULT_VARIABLE _rc)
        endif()
    else()
        execute_process(COMMAND git -C "${_ar}" rev-parse HEAD
                        OUTPUT_VARIABLE _head OUTPUT_STRIP_TRAILING_WHITESPACE
                        ERROR_QUIET RESULT_VARIABLE _rc)
        if(_rc EQUAL 0 AND _head STREQUAL _pin)
            return()
        endif()
        message(WARNING "cyber_quadcover_solver: vendored AutoRemesher checkout at "
                        "'${_head}' does not match pin ${_pin} — re-fetching the pinned "
                        "commit (a stale checkout builds a DIFFERENT field solver)")
        set(_rc 0)
    endif()
    if(_rc EQUAL 0)
        execute_process(COMMAND git -C "${_ar}" fetch --depth 1 origin "${_pin}"
                        RESULT_VARIABLE _rc)
    endif()
    if(_rc EQUAL 0)
        execute_process(COMMAND git -C "${_ar}" checkout --force --detach FETCH_HEAD
                        RESULT_VARIABLE _rc)
    endif()
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "cyber_quadcover_solver: failed to fetch AutoRemesher at "
                            "pinned commit ${_pin}")
    endif()
endfunction()

# Record which field solver this build tree actually contains, so a packaged
# artifact can be identified after the fact. The packaging scripts copy the file
# into the artifact; without it, whether a release carries the vendored Geogram
# field or the native fallback was decided by whatever the runner image happened
# to ship, and nothing downstream could tell which.
function(cyber_record_field_solver _solver)
    file(WRITE "${PROJECT_BINARY_DIR}/cyber_field_solver.txt" "${_solver}\n")
    set_property(GLOBAL PROPERTY CYBER_FIELD_SOLVER "${_solver}")
endfunction()

function(cyber_add_quadcover_solver)
    set(_ref "${PROJECT_SOURCE_DIR}/examples/reference")
    set(_ar "${_ref}/autoremesher-src")

    # The vendored Geogram/AutoRemesher solver needs OpenMP and TBB. Where either
    # is absent (a minimal CI runner, a macOS box without libomp), skip it and
    # let the dependency-free native seamless-UV solver take over. This keeps
    # -DCYBER_WITH_QUADCOVER=ON portable — best-effort, never a hard configure
    # failure — so the tree builds everywhere and the vendored field is used only
    # where its dependencies are present.
    #
    # The TBB *library* is searched too, not just its headers: the link line below
    # names `tbb` directly, so a box with the headers but no runtime used to
    # configure happily and die at link time.
    find_package(OpenMP QUIET)
    # Homebrew's libomp is keg-only, so it is not on AppleClang's default search
    # path and the plain find_package above misses it even when it is installed.
    # Retry with the brew prefix before giving up, so the "brew libomp tbb" the
    # error below recommends is actually enough on macOS -- it was not, and the
    # message sent you to install packages you already had.
    if(APPLE AND NOT OpenMP_CXX_FOUND)
        execute_process(COMMAND brew --prefix libomp
            OUTPUT_VARIABLE _brew_libomp OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET RESULT_VARIABLE _brew_rc)
        if(_brew_rc EQUAL 0 AND EXISTS "${_brew_libomp}")
            set(OpenMP_ROOT "${_brew_libomp}")
            find_package(OpenMP QUIET)
        endif()
    endif()
    find_path(CYBER_QC_TBB_INCLUDE tbb/blocked_range.h)
    find_library(CYBER_QC_TBB_LIB NAMES tbb)
    if(NOT OpenMP_CXX_FOUND OR NOT CYBER_QC_TBB_INCLUDE OR NOT CYBER_QC_TBB_LIB)
        # A release lane must choose its field solver rather than inherit it from
        # the runner image, so those lanes set CYBER_REQUIRE_QUADCOVER=ON and the
        # portable fallback becomes a hard error.
        if(CYBER_REQUIRE_QUADCOVER)
            message(FATAL_ERROR
                "CYBER_REQUIRE_QUADCOVER=ON but the in-process Geogram field cannot "
                "be built: OpenMP_CXX_FOUND=${OpenMP_CXX_FOUND}, "
                "tbb headers=${CYBER_QC_TBB_INCLUDE}, tbb library=${CYBER_QC_TBB_LIB}. "
                "Install OpenMP + TBB (apt libtbb-dev / brew libomp tbb), or drop "
                "-DCYBER_REQUIRE_QUADCOVER=ON to accept the native seamless-UV solver.")
        endif()
        message(STATUS "cyber_quadcover_solver: OpenMP/TBB not found — skipping the "
                       "in-process Geogram field; using the native seamless-UV solver")
        cyber_record_field_solver("native-seamless-uv")
        return()
    endif()

    # The AutoRemesher/Geogram sources are vendored on demand at the exact
    # commit recorded in autoremesher.pin (single source of truth, shared with
    # build_autoremesher.sh), so a clean checkout can configure with
    # -DCYBER_WITH_QUADCOVER=ON and always builds the same field solver.
    file(READ "${_ref}/autoremesher.pin" _pin)
    string(STRIP "${_pin}" _pin)
    cyber_fetch_pinned_autoremesher("${_ar}" "${_pin}")

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
    # No bare -fopenmp here: build_autoremesher.sh could hardcode it because it only
    # ever ran under GCC, but AppleClang rejects the spelling outright ("unsupported
    # option '-fopenmp'") and needs -Xclang -fopenmp. The OpenMP::OpenMP_CXX target
    # linked below already carries whatever the detected compiler wants, so passing it
    # again by hand is redundant everywhere and fatal on macOS.
    target_compile_options(cyber_quadcover_solver PRIVATE -w -pthread)
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
    # find_path already located the TBB headers for the capability check above, but
    # the result was never handed to the compiler. On Linux that went unnoticed
    # because they sit in /usr/include; Homebrew's tbb is keg-only, so the vendored
    # sources failed on `tbb/blocked_range.h` with the headers plainly present.
    # SYSTEM so TBB's own warnings stay out of the build log.
    target_include_directories(cyber_quadcover_solver SYSTEM PRIVATE
        "${CYBER_QC_TBB_INCLUDE}")
    # The solve TU's public header lives next to it; expose it so consumers can
    # include "autoremesher_solve.hpp" without seeing any Geogram include path.
    target_include_directories(cyber_quadcover_solver PUBLIC "${_ref}")

    # Resolved paths rather than bare `-ltbb -lz`: both become DT_NEEDED entries on
    # every binary that links this archive, so they are real runtime dependencies
    # and the packaging step has to bundle them (packaging/linux/appimage.sh).
    find_package(ZLIB REQUIRED)
    find_package(OpenMP REQUIRED)
    target_link_libraries(cyber_quadcover_solver PRIVATE
        OpenMP::OpenMP_CXX "${CYBER_QC_TBB_LIB}" ZLIB::ZLIB ${CMAKE_DL_LIBS} m)

    cyber_record_field_solver("quadcover-geogram-1.8.3+autoremesher@${_pin}")
endfunction()
