# TODO: why is this add_compile_options instead of target?
add_compile_options(-fno-operator-names)

add_library(LinuxEmulation STATIC
  VDSO_Emulation.cpp
  Thunks.cpp
  GdbServer/Info.cpp
  LinuxSyscalls/GdbServer.cpp
  LinuxSyscalls/GranuleMemory.cpp
  LinuxSyscalls/GranuleTable.cpp
  LinuxSyscalls/HostOwnedRanges.cpp
  LinuxSyscalls/EmulatedFiles/EmulatedFiles.cpp
  LinuxSyscalls/FaultSafeUserMemAccess.cpp
  LinuxSyscalls/FileManagement.cpp
  LinuxSyscalls/LinuxAllocator.cpp
  LinuxSyscalls/Seccomp/SeccompEmulator.cpp
  LinuxSyscalls/Seccomp/BPFInterpreter.cpp
  LinuxSyscalls/Seccomp/Dumper.cpp
  LinuxSyscalls/SignalDelegator.cpp
  LinuxSyscalls/SMCStoreBackpatch.cpp
  LinuxSyscalls/Syscalls.cpp
  LinuxSyscalls/SyscallsSMCTracking.cpp
  LinuxSyscalls/SyscallsVMATracking.cpp
  LinuxSyscalls/SyscallObserver.cpp
  LinuxSyscalls/CoreIsolation.cpp
  LinuxSyscalls/ThreadCensus.cpp
  LinuxSyscalls/ThreadManager.cpp
  LinuxSyscalls/SignalDelegator/GuestFramesManagement.cpp
  LinuxSyscalls/Utils/Threads.cpp
  LinuxSyscalls/Arm64/Syscalls.cpp
  LinuxSyscalls/Syscalls/EPoll.cpp
  LinuxSyscalls/Syscalls/FD.cpp
  LinuxSyscalls/Syscalls/FS.cpp
  LinuxSyscalls/Syscalls/Passthrough.cpp
  LinuxSyscalls/Syscalls/Info.cpp
  LinuxSyscalls/Syscalls/IO.cpp
  LinuxSyscalls/Syscalls/Memory.cpp
  LinuxSyscalls/Syscalls/Signals.cpp
  LinuxSyscalls/Syscalls/Thread.cpp
  LinuxSyscalls/Syscalls/Timer.cpp
  LinuxSyscalls/Syscalls/NotImplemented.cpp
  LinuxSyscalls/Syscalls/Stubs.cpp)

target_compile_options(LinuxEmulation PRIVATE
  -Wall
  -Werror=cast-qual
  -Werror=ignored-qualifiers
  -Werror=implicit-fallthrough

  -Wno-trigraphs
  -fwrapv)

set_target_properties(LinuxEmulation PROPERTIES
  C_VISIBILITY_PRESET hidden
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN TRUE)

target_include_directories(LinuxEmulation PRIVATE
  ${CMAKE_BINARY_DIR}/generated
  ${CMAKE_CURRENT_SOURCE_DIR}/
  ${PROJECT_SOURCE_DIR}/External/drm-headers/include/)

target_include_directories(LinuxEmulation INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/)

target_link_libraries(LinuxEmulation PRIVATE
  Common
  CommonTools
  CodeEmitter)

target_link_libraries(LinuxEmulation INTERFACE FEXCore)

# The arm64 guest layouts are not hand-written headers: gen_abi_tables.py
# derives them from the kernel uapi headers for both targets and
# Arm64/Tests checks them against natively compiled references, so there is
# nothing for StructPackVerifier to parse.
set(HEADERS_TO_VERIFY
)

if (BUILD_TESTING)
  # Host-side tests of the arm64 -> ppc64le translators (native, no guest code).
  add_executable(POWERarmABITranslationTests LinuxSyscalls/Arm64/Tests/ABITranslationTests.cpp)
  target_include_directories(POWERarmABITranslationTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/
    ${CMAKE_CURRENT_SOURCE_DIR}/LinuxSyscalls/Arm64/Tests/)
  target_compile_options(POWERarmABITranslationTests PRIVATE -Wall -Werror)
  add_test(NAME POWERarm/ABITranslation COMMAND POWERarmABITranslationTests)

  # Optional: the committed tables must match a regeneration from the kernel tree.
  set(POWERARM_UAPI_KERNEL_TREE "" CACHE PATH "Kernel source tree GeneratedABI.h was generated from")
  if (POWERARM_UAPI_KERNEL_TREE)
    add_test(NAME POWERarm/ABITablesCurrent
      COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/LinuxSyscalls/Arm64/gen_abi_tables.py --check
        --kernel ${POWERARM_UAPI_KERNEL_TREE}
        --header ${CMAKE_CURRENT_SOURCE_DIR}/LinuxSyscalls/Arm64/GeneratedABI.h
        --doc ${PROJECT_SOURCE_DIR}/docs/powerarm/M1-ABI-DIFFERENCES.md
        --names ${CMAKE_CURRENT_SOURCE_DIR}/LinuxSyscalls/Arm64/Tests/ConstantNames.txt)
  endif()
endif()

list(LENGTH HEADERS_TO_VERIFY ARG_COUNT)
math(EXPR ARG_COUNT "${ARG_COUNT}-1")

set(ARGS
  "-x" "c++"
  "-std=c++20"
  "-fno-operator-names"
  "-I${PROJECT_SOURCE_DIR}/External/drm-headers/include/")

# Global include directories
get_directory_property (INC_DIRS INCLUDE_DIRECTORIES)
list(TRANSFORM INC_DIRS PREPEND "-I")
list(APPEND ARGS ${INC_DIRS})

# FEXCore directories
get_target_property(INC_DIRS FEXCore INTERFACE_INCLUDE_DIRECTORIES)
list(TRANSFORM INC_DIRS PREPEND "-I")
list(APPEND ARGS ${INC_DIRS})

get_target_property(INC_DIRS LinuxEmulation INTERFACE_INCLUDE_DIRECTORIES)
list(TRANSFORM INC_DIRS PREPEND "-I")
list(APPEND ARGS ${INC_DIRS})

# StructPackVerifier uses clang's Python bindings to cross-parse `x32/*.h` as
# x86_64 and aarch64 to compare the FEX-emulator-side struct layouts against
# the expected guest layouts. On PPC64LE hosts, clang's default install does
# not ship cross-compile c++ headers (only powerpc64le-unknown-linux-gnu
# c++config.h exists; x86_64/aarch64 equivalents would need separate
# cross-sysroot packages). Without those, every clang parse returns -1 for
# every field offset and all 18 tests fail spuriously. Gate the tests off
# for hosts that don't have the cross-compile environment.
if (ARCHITECTURE_ppc64le)
  message(STATUS "StructPackVerifier: skipped on PPC64LE (requires x86_64+aarch64 cross-compile c++ headers; install via separate cross-sysroot if needed)")
else()
foreach(Index RANGE 0 ${ARG_COUNT} 2)
  math(EXPR TEST_TYPE_INDEX "${Index}+1")

  list(GET HEADERS_TO_VERIFY ${Index} HEADER)
  list(GET HEADERS_TO_VERIFY ${TEST_TYPE_INDEX} TEST_TYPE)

  file(RELATIVE_PATH REL_HEADER ${CMAKE_BINARY_DIR} "${CMAKE_CURRENT_SOURCE_DIR}/${HEADER}")
  set(TEST_NAME "${TEST_DESC}/Test_verify_${HEADER}")
  set(TEST_NAME_ARCH "${TEST_DESC}/Test_verify_arch_${HEADER}")

  add_test(NAME ${TEST_NAME}_x86_64
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMAND "python3" "${CMAKE_SOURCE_DIR}/Scripts/StructPackVerifier.py" "-c1" "x86_64" "${REL_HEADER}" ${ARGS})

  add_test(NAME ${TEST_NAME}_aarch64
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMAND "python3" "${CMAKE_SOURCE_DIR}/Scripts/StructPackVerifier.py" "-c1" "aarch64" "${REL_HEADER}" ${ARGS})

  add_test(NAME ${TEST_NAME_ARCH}_x86_64
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMAND "python3" "${CMAKE_SOURCE_DIR}/Scripts/StructPackVerifier.py" "-c1" "x86_64" "-c2" "${TEST_TYPE}" "${REL_HEADER}" ${ARGS})

  add_test(NAME ${TEST_NAME_ARCH}_aarch64
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMAND "python3" "${CMAKE_SOURCE_DIR}/Scripts/StructPackVerifier.py" "-c1" "aarch64" "-c2" "${TEST_TYPE}" "${REL_HEADER}" ${ARGS})

  set_property(TEST ${TEST_NAME}_x86_64 APPEND PROPERTY DEPENDS "${HEADER}")
  set_property(TEST ${TEST_NAME}_aarch64 APPEND PROPERTY DEPENDS "${HEADER}")
  set_property(TEST ${TEST_NAME_ARCH}_x86_64 APPEND PROPERTY DEPENDS "${HEADER}")
  set_property(TEST ${TEST_NAME_ARCH}_aarch64 APPEND PROPERTY DEPENDS "${HEADER}")
endforeach()
endif()

add_custom_target(struct_verifier
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
  USES_TERMINAL
  COMMAND "ctest" "--output-on-failure" "--timeout" "302" ${TEST_JOB_FLAG} "-R" "Test_verify*")
