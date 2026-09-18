# AArch64 guest code (ThunkLibs/GuestLibs), compiled by the guest rootfs's own
# gcc running under POWERarm. GUEST_TOOLCHAIN_DIR holds the aarch64-gcc and
# aarch64-g++ wrappers the top-level CMakeLists generates; they exec
# `POWERarm /usr/bin/gcc` with POWERARM_ROOTFS pinned to GUEST_ROOTFS, so the
# compiler, its headers and libraries, binutils and the CRT objects are all the
# rootfs's, and no sysroot flag is needed.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# try_compile() re-reads this file in a fresh CMake invocation.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES GUEST_TOOLCHAIN_DIR GUEST_ROOTFS)

if (NOT GUEST_TOOLCHAIN_DIR OR NOT EXISTS "${GUEST_TOOLCHAIN_DIR}/aarch64-gcc")
  message(FATAL_ERROR "GUEST_TOOLCHAIN_DIR must hold the aarch64-gcc/aarch64-g++ wrappers (got '${GUEST_TOOLCHAIN_DIR}')")
endif()

set(CMAKE_C_COMPILER "${GUEST_TOOLCHAIN_DIR}/aarch64-gcc")
set(CMAKE_CXX_COMPILER "${GUEST_TOOLCHAIN_DIR}/aarch64-g++")

# Never look at the host's (ppc64le) libraries or headers for guest code.
set(CMAKE_FIND_ROOT_PATH "${GUEST_ROOTFS}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
