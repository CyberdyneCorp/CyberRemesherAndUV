# UNVERIFIED: release packaging helper, not part of the core CI build.
#
# CMake-side companion to packaging/version.sh. Standalone packaging steps that
# configure CMake outside the normal build (e.g. CPack driver scripts) can
# include this to obtain the same single-source version without duplicating it.
#
# The authoritative value remains the root project(... VERSION ...) field; this
# module simply re-reads it so packaging tooling and binaries never diverge
# (build-and-packaging spec: "Single version identity").
#
#   include(packaging/version.cmake)
#   cyber_read_version(CYBER_VERSION)
#   message(STATUS "Packaging CyberRemesher ${CYBER_VERSION}")

# Captured at include time so the default repo-root resolves relative to this
# module regardless of which file later calls the functions below.
set(CYBER_PACKAGING_DIR "${CMAKE_CURRENT_LIST_DIR}" CACHE INTERNAL "packaging dir")

# Read "VERSION x.y.z" from the given (or root) CMakeLists into out_var.
function(cyber_read_version out_var)
    set(_root "${ARGN}")
    if(NOT _root)
        set(_root "${CYBER_PACKAGING_DIR}/..")
    endif()
    file(READ "${_root}/CMakeLists.txt" _contents)
    string(REGEX MATCH "VERSION[ \t\r\n]+([0-9]+\\.[0-9]+\\.[0-9]+)" _m "${_contents}")
    if(NOT CMAKE_MATCH_1)
        message(FATAL_ERROR "cyber_read_version: no project VERSION found in ${_root}/CMakeLists.txt")
    endif()
    set(${out_var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

# Compose a versioned artifact filename matching packaging/version.sh.
#   cyber_artifact_name(out "macos" "dmg")        -> cyberremesh-0.1.0-macos.dmg
#   cyber_artifact_name(out "windows" "zip" "x64")-> cyberremesh-0.1.0-windows-x64.zip
function(cyber_artifact_name out_var platform ext)
    cyber_read_version(_ver)
    set(_suffix "${ARGN}")
    if(_suffix)
        set(${out_var} "cyberremesh-${_ver}-${platform}-${_suffix}.${ext}" PARENT_SCOPE)
    else()
        set(${out_var} "cyberremesh-${_ver}-${platform}.${ext}" PARENT_SCOPE)
    endif()
endfunction()
