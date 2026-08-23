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
#
# Deliberately NO CMAKE_SYSROOT for the distro toolchain. Debian/Ubuntu ship
# /usr/aarch64-linux-gnu/lib/libc.so as a linker script that names its
# dependencies by ABSOLUTE path. Passing --sysroot=/usr/aarch64-linux-gnu makes
# ld re-root those names, so it hunts for
#   /usr/aarch64-linux-gnu/usr/aarch64-linux-gnu/lib/libm.so.6
# and the compiler check fails with "cannot find .../libm.so.6 inside
# /usr/aarch64-linux-gnu". The cross compiler already knows its own library and
# header paths; a multiarch cross toolchain must not be given a sysroot.
if(DEFINED CYBER_AARCH64_SYSROOT)
    set(CMAKE_SYSROOT ${CYBER_AARCH64_SYSROOT})
    set(_cyber_guest_libs ${CYBER_AARCH64_SYSROOT})
else()
    set(_cyber_guest_libs /usr/${_cyber_triple})
endif()

# qemu-user resolves the guest's shared libraries under -L.
if(NOT DEFINED CYBER_QEMU_AARCH64)
    set(CYBER_QEMU_AARCH64 qemu-aarch64-static)
endif()
set(CMAKE_CROSSCOMPILING_EMULATOR ${CYBER_QEMU_AARCH64} -L ${_cyber_guest_libs})

# Look for programs on the host, libraries and headers in the target tree only.
# With no CMAKE_SYSROOT this is what keeps find_library() off the host's x86
# libraries, so it has to be set explicitly.
set(CMAKE_FIND_ROOT_PATH ${_cyber_guest_libs})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
