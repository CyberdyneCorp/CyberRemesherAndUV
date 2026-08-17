# Cross-compile to 64-bit ARM (Linux/glibc) with the distro's
# g++-aarch64-linux-gnu, and run the resulting binaries under qemu-user.
#
# This is the only lane that actually EXECUTES the engine on the word size,
# alignment and (crucially) the signed-vs-unsigned `char` of the mobile targets;
# x86 CI cannot see a bug that depends on any of them. It is not iOS or Android
# — no NDK, no Apple SDK — but it is the same ISA, and it needs nothing but two
# apt packages, so it can gate every pull request.
#
# CMAKE_CROSSCOMPILING_EMULATOR makes add_test() run each binary under qemu, so
# `ctest` works without binfmt registration (i.e. without root).

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(_cyber_triple aarch64-linux-gnu)

if(NOT DEFINED CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER ${_cyber_triple}-gcc)
endif()
if(NOT DEFINED CMAKE_CXX_COMPILER)
    set(CMAKE_CXX_COMPILER ${_cyber_triple}-g++)
endif()

# CYBER_AARCH64_SYSROOT / CYBER_QEMU_AARCH64 let a toolchain unpacked somewhere
# other than /usr (an unprivileged CI cache, say) be used unchanged.
if(DEFINED CYBER_AARCH64_SYSROOT)
    set(CMAKE_SYSROOT ${CYBER_AARCH64_SYSROOT})
elseif(EXISTS /usr/${_cyber_triple})
    set(CMAKE_SYSROOT /usr/${_cyber_triple})
endif()

if(NOT DEFINED CYBER_QEMU_AARCH64)
    set(CYBER_QEMU_AARCH64 qemu-aarch64-static)
endif()
set(CMAKE_CROSSCOMPILING_EMULATOR ${CYBER_QEMU_AARCH64} -L ${CMAKE_SYSROOT})

# Look for programs on the host, libraries and headers in the sysroot only.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
