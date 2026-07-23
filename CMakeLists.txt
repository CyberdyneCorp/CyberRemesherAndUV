cmake_minimum_required(VERSION 3.24)

project(CyberRemesher
    VERSION 0.2.2
    DESCRIPTION "Quad remeshing, retopology, UV and baking engine"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# Static libs may be linked into the C ABI shared library (capi), so build
# everything position-independent.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(CYBER_BUILD_TESTS "Build unit tests" ON)
option(CYBER_BUILD_CLI "Build the headless CLI" ON)
option(CYBER_BUILD_RENDER "Build the viewport renderer" OFF)
option(CYBER_BUILD_APPS "Build desktop/mobile application shells" OFF)
option(CYBER_BUILD_NET "Build the local network bridge" ON)
option(CYBER_BUILD_CAPI "Build the C ABI facade over the pipeline" ON)
# Library modules (document/undo/input, UV editing, retopology geometry, bake
# cage) — build + tested clean under the strict flags, so ON by default. Their
# GUI/interaction subtasks remain best-effort (see tasks.md).
option(CYBER_BUILD_APP "Build the application-shell library (document/undo/input)" ON)
option(CYBER_BUILD_UV "Build the UV editing module (seams, LSCM unwrap, packing)" ON)
option(CYBER_BUILD_RETOPO "Build the manual-retopology toolset" ON)
option(CYBER_BUILD_BAKECAGE "Build the editable projection cage + component links" ON)
option(CYBER_ENABLE_METAL "Enable the Metal compute backend" OFF)
option(CYBER_ENABLE_CUDA "Enable the CUDA compute backend" OFF)
option(CYBER_ENABLE_OPENCL "Enable the OpenCL compute backend" OFF)
# In-process QuadCover seamless-UV solver (Geogram + AutoRemesher). OFF by default:
# ON vendors and compiles a large Geogram subset (see cmake/QuadCoverSolver.cmake)
# and links it into cyber_quadrangulate so the quad-cover method needs no external
# CYBER_QUADCOVER_CLI harness. Kept off for normal builds to avoid the dependency.
option(CYBER_WITH_QUADCOVER "Build the in-process QuadCover seamless-UV solver" OFF)

# Vendors NumPP + SciPP (github.com/CyberdyneCorp) and links scipp::scipp into
# cyber_quadrangulate for the min-cost-flow integer quadrangulator that ports
# QuadriFlow's integer-layout stage (docs/mcf-integer-layout-plan.md). Off by
# default so normal builds stay dependency-free.
option(CYBER_WITH_SCIPP "Build the min-cost-flow integer quadrangulator (vendored NumPP+SciPP)" OFF)

include(cmake/CompilerWarnings.cmake)

if(CYBER_WITH_QUADCOVER)
    # The vendored Geogram subset includes ~11 C translation units; the base
    # project only enables CXX, so enable C for the isolated solver library.
    enable_language(C)
    include(cmake/QuadCoverSolver.cmake)
endif()

if(CYBER_WITH_SCIPP)
    include(cmake/SciPP.cmake)
    cyber_add_scipp()
endif()

add_subdirectory(src/core)
add_subdirectory(src/accel)
add_subdirectory(src/bake)
# imageio depends on bake (saveImage) and bake uses imageio's header (texture
# baking) — always built, right after bake, so both resolve.
add_subdirectory(src/imageio)
add_subdirectory(src/quadrangulate)
# UV module before capi so the C ABI can conditionally link cyber_uv (atlas).
if(CYBER_BUILD_UV)
    add_subdirectory(src/uv)
endif()

if(CYBER_BUILD_CAPI)
    add_subdirectory(capi)
endif()
if(CYBER_BUILD_RENDER)
    add_subdirectory(src/render)
endif()
if(CYBER_BUILD_APP)
    add_subdirectory(src/app)
endif()
if(CYBER_BUILD_RETOPO)
    add_subdirectory(src/retopo)
endif()
if(CYBER_BUILD_BAKECAGE)
    add_subdirectory(src/bakecage)
endif()

# The local network bridge uses POSIX sockets; a Winsock shim is a follow-up.
if(CYBER_BUILD_NET AND NOT WIN32)
    add_subdirectory(src/net)
    add_subdirectory(apps/bridge)
endif()

if(CYBER_BUILD_CLI)
    add_subdirectory(apps/cli)
endif()

if(CYBER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Install / export — find_package(CyberRemesher CONFIG) -> cyber::capi
# ---------------------------------------------------------------------------
# Downstream projects (like the sibling CyberdyneCorp libraries) consume the
# stable C ABI shared library as an imported target. The C++ core, quadrangulate,
# UV, and the optional in-process Geogram solver are all PRIVATE-linked into the
# .so, so exporting only cyber_capi_shared yields a self-contained package.
if(CYBER_BUILD_CAPI AND CYBER_BUILD_CAPI_SHARED)
    include(CMakePackageConfigHelpers)
    include(GNUInstallDirs)
    set(CYBER_CMAKE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/CyberRemesher")

    install(EXPORT CyberRemesherTargets
        NAMESPACE cyber::
        DESTINATION "${CYBER_CMAKE_DIR}"
        FILE CyberRemesherTargets.cmake)

    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CyberRemesherConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/CyberRemesherConfig.cmake"
        INSTALL_DESTINATION "${CYBER_CMAKE_DIR}")

    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/CyberRemesherConfigVersion.cmake"
        VERSION "${PROJECT_VERSION}"
        COMPATIBILITY SameMajorVersion)

    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/CyberRemesherConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/CyberRemesherConfigVersion.cmake"
        DESTINATION "${CYBER_CMAKE_DIR}")
endif()
