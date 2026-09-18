cmake_minimum_required(VERSION 3.14)
project(guest-thunks)

# AArch64 guest stub libraries. Configured by the top-level ExternalProject
# `guest-libs` with Data/CMake/toolchain_aarch64_guest.cmake: the compiler is
# the guest rootfs's gcc running under POWERarm.

if (NOT GENERATOR_EXE OR NOT GUEST_ROOTFS OR NOT FEX_PROJECT_SOURCE_DIR)
  message(FATAL_ERROR "Configure through the top-level project (BUILD_THUNKS, BUILD_GUEST_THUNKS)")
endif()

set(CMAKE_CXX_STANDARD 20)

set(DATA_DIRECTORY "" CACHE PATH "Global data directory (override)")
if (NOT DATA_DIRECTORY)
  set(DATA_DIRECTORY "${CMAKE_INSTALL_PREFIX}/share/powerarm")
endif()

# Syntax: generate(libxyz libxyz-interface.cpp)
# Runs thunkgen on the interface definition. libxyz-guest-deps is an interface
# target for include directories and definitions that the interface parse and
# the stub compile share.
function(generate NAME SOURCE_FILE)
  add_library(${NAME}-guest-deps INTERFACE)
  target_include_directories(${NAME}-guest-deps INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/../include")
  set(prop "$<TARGET_PROPERTY:${NAME}-guest-deps,INTERFACE_INCLUDE_DIRECTORIES>")
  set(compile_prop "$<TARGET_PROPERTY:${NAME}-guest-deps,INTERFACE_COMPILE_DEFINITIONS>")

  set(OUTFOLDER "${CMAKE_CURRENT_BINARY_DIR}/gen")
  set(OUTFILE "${OUTFOLDER}/thunkgen_guest_${NAME}.inl")
  file(MAKE_DIRECTORY "${OUTFOLDER}")

  # thunkgen adds --target=aarch64-linux-gnu and --sysroot=GUEST_ROOTFS to
  # both of its parses (ThunkLibs/Generator/main.cpp).
  add_custom_command(
    OUTPUT "${OUTFILE}"
    DEPENDS "${GENERATOR_EXE}"
    DEPENDS "${SOURCE_FILE}"
    COMMAND "${GENERATOR_EXE}" "${SOURCE_FILE}" "${NAME}" "-guest" "${OUTFILE}" "${GUEST_ROOTFS}" -- -std=c++20
      "$<$<BOOL:${compile_prop}>:;-D$<JOIN:${compile_prop},;-D>>"
      "$<$<BOOL:${prop}>:;-isystem$<JOIN:${prop},;-isystem>>"
    VERBATIM
    COMMAND_EXPAND_LISTS)

  set(GEN_${NAME} "${OUTFILE}" PARENT_SCOPE)
endfunction()

function(add_guest_lib NAME SONAME)
  set(SOURCE_FILE_ABS "${CMAKE_CURRENT_SOURCE_DIR}/../lib${NAME}/lib${NAME}_Guest.cpp")
  if (NOT EXISTS "${SOURCE_FILE_ABS}")
    set(SOURCE_FILE_ABS "${CMAKE_CURRENT_SOURCE_DIR}/../lib${NAME}/Guest.cpp")
    if (NOT EXISTS "${SOURCE_FILE_ABS}")
      message(FATAL_ERROR "Thunk source file for Guest lib ${NAME} doesn't exist!")
    endif()
  endif()
  set(SOURCE_LDS_FILE_ABS "${CMAKE_CURRENT_SOURCE_DIR}/../lib${NAME}/lib${NAME}_Guest.lds")

  add_library(${NAME}-guest SHARED "${SOURCE_FILE_ABS}" ${GEN_lib${NAME}})
  target_include_directories(${NAME}-guest PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/gen/")
  target_compile_definitions(${NAME}-guest PRIVATE GUEST_THUNK_LIBRARY)
  target_link_libraries(${NAME}-guest PRIVATE lib${NAME}-guest-deps)

  ## Make signed overflow well defined 2's complement overflow
  target_compile_options(${NAME}-guest PRIVATE -fwrapv)

  if (EXISTS "${SOURCE_LDS_FILE_ABS}")
    target_link_options(${NAME}-guest PRIVATE "-T" "${SOURCE_LDS_FILE_ABS}")
    set_property(TARGET ${NAME}-guest APPEND PROPERTY LINK_DEPENDS "${SOURCE_LDS_FILE_ABS}")
  endif()

  # Guest thunk libraries are named lib<Thunk>-guest.so but carry the soname of
  # the library they replace, so that a later dlopen by soname (RTLD_NOLOAD
  # included) finds the stub. The vDSO is only ever looked up by soname.
  target_link_options(${NAME}-guest PRIVATE "LINKER:-soname,${SONAME}")
  set_target_properties(${NAME}-guest PROPERTIES NO_SONAME ON)

  install(TARGETS ${NAME}-guest DESTINATION ${DATA_DIRECTORY}/GuestThunks/)
endfunction()

# The linked-callee convention (common/Guest.h, CallHostFunction) depends on
# code generation: each invoker must copy X17 before anything can clobber it.
# Compile a probe of representative invokers with this guest compiler and fail
# the build if any does not. Needs the host's llvm-objdump and python3.
find_program(GUEST_THUNK_OBJDUMP NAMES llvm-objdump)
find_program(GUEST_THUNK_PYTHON NAMES python3)
if (GUEST_THUNK_OBJDUMP AND GUEST_THUNK_PYTHON)
  add_library(linked-callee-probe OBJECT linked_callee_probe.cpp)
  target_include_directories(linked-callee-probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../include")
  target_compile_options(linked-callee-probe PRIVATE -fPIC)
  add_custom_target(linked-callee-check ALL
    COMMAND "${GUEST_THUNK_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/check_linked_callee.py" "${GUEST_THUNK_OBJDUMP}"
            "$<TARGET_OBJECTS:linked-callee-probe>"
    DEPENDS linked-callee-probe "${CMAKE_CURRENT_SOURCE_DIR}/check_linked_callee.py"
    VERBATIM
    COMMAND_EXPAND_LISTS)
else()
  message(WARNING "llvm-objdump or python3 not found: the linked-callee (X17) check is skipped")
endif()

# The vDSO. Shaped like the arm64 kernel's (arch/arm64/kernel/vdso): the
# __kernel_* entry points under version LINUX_2.6.39, which is what glibc and
# musl look up, plus __kernel_rt_sigreturn for signal return.
generate(libVDSO ${CMAKE_CURRENT_SOURCE_DIR}/../libVDSO/libVDSO_interface.cpp)
add_guest_lib(VDSO "linux-vdso.so.1")
# libVDSO-a64-guest.so, the name VDSO_Emulation.cpp looks for: no POWERarm
# older than the thunk marker loads a file of that name (see there).
set_target_properties(VDSO-guest PROPERTIES OUTPUT_NAME "VDSO-a64-guest")
# POWERarm maps the file whole (VDSO_Emulation.cpp LoadVDSOThunks) and nothing
# applies relocations to it, and nothing in it may need a libc. So: no stack
# protector (it reads a guard through the GOT), no unwind tables (the linker
# script discards them), no outline-atomics helpers, and a link that fails on
# any undefined symbol. `readelf -r` on the result must list no relocations.
target_compile_options(VDSO-guest PRIVATE -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables
                                          -fno-exceptions -mno-outline-atomics)
target_link_options(VDSO-guest PRIVATE "-nostdlib" "LINKER:--no-undefined" "LINKER:-z,max-page-size=4096" "LINKER:--hash-style=both"
                                       "LINKER:-z,text")
