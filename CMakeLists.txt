cmake_minimum_required(VERSION 3.24)

project(CyberRemesher
    VERSION 0.1.0
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
option(CYBER_BUILD_IMAGEIO "Build the imageio PNG/EXR/ZIP map & package export module" ON)
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

include(cmake/CompilerWarnings.cmake)

add_subdirectory(src/core)
add_subdirectory(src/accel)
add_subdirectory(src/bake)
add_subdirectory(src/quadrangulate)

if(CYBER_BUILD_IMAGEIO)
    add_subdirectory(src/imageio)
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
if(CYBER_BUILD_UV)
    add_subdirectory(src/uv)
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
