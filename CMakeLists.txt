cmake_minimum_required(VERSION 3.24)

project(CyberRemesher
    VERSION 0.1.0
    DESCRIPTION "Quad remeshing, retopology, UV and baking engine"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(CYBER_BUILD_TESTS "Build unit tests" ON)
option(CYBER_BUILD_CLI "Build the headless CLI" ON)
option(CYBER_BUILD_RENDER "Build the viewport renderer" OFF)
option(CYBER_BUILD_APPS "Build desktop/mobile application shells" OFF)
option(CYBER_BUILD_NET "Build the local network bridge" ON)
option(CYBER_ENABLE_METAL "Enable the Metal compute backend" OFF)
option(CYBER_ENABLE_CUDA "Enable the CUDA compute backend" OFF)
option(CYBER_ENABLE_OPENCL "Enable the OpenCL compute backend" OFF)

include(cmake/CompilerWarnings.cmake)

add_subdirectory(src/core)
add_subdirectory(src/accel)

# The local network bridge uses POSIX sockets; a Winsock shim is a follow-up.
if(CYBER_BUILD_NET AND NOT WIN32)
    add_subdirectory(src/net)
endif()

if(CYBER_BUILD_CLI)
    add_subdirectory(apps/cli)
endif()

if(CYBER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
