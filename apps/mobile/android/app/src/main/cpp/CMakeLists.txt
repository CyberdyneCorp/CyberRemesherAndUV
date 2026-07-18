# UNVERIFIED: Android shell NDK build (task 8.5).
#
# This is the *app's own* native build script, invoked by Gradle's
# externalNativeBuild — it is NOT part of the CyberRemesher engine CMake tree and
# is never included by the repo root CMakeLists. It builds only the thin JNI
# shim (cyber_jni.cpp) and links the pre-built libcyber_capi produced by
# cross-compiling the engine for Android.
#
# Not built in CI: requires the Android NDK and a cross-compiled libcyber_capi
# per ABI. Point CYBER_CAPI_DIR at the directory holding cyber_capi.h and
# lib<abi>/libcyber_capi.so (passed from app/build.gradle.kts).

cmake_minimum_required(VERSION 3.22.1)
project(cyber_shell LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED CYBER_CAPI_DIR)
    message(FATAL_ERROR "CYBER_CAPI_DIR is not set: point it at the cross-compiled capi (headers + libcyber_capi.so per ABI).")
endif()

add_library(cyber_shell SHARED cyber_jni.cpp)

target_include_directories(cyber_shell PRIVATE
    "${CYBER_CAPI_DIR}/include"
)

# Per-ABI prebuilt engine library. ANDROID_ABI is provided by the NDK toolchain.
find_library(CYBER_CAPI_LIB
    NAMES cyber_capi
    PATHS "${CYBER_CAPI_DIR}/lib/${ANDROID_ABI}"
    NO_DEFAULT_PATH
)
if(NOT CYBER_CAPI_LIB)
    message(FATAL_ERROR "libcyber_capi not found for ABI ${ANDROID_ABI} under ${CYBER_CAPI_DIR}/lib/${ANDROID_ABI}")
endif()

find_library(ANDROID_LIB android)
find_library(LOG_LIB log)

target_link_libraries(cyber_shell
    PRIVATE ${CYBER_CAPI_LIB} ${ANDROID_LIB} ${LOG_LIB}
)
