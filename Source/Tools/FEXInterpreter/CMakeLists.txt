list(APPEND LIBS FEXCore Common JemallocLibs LinuxEmulation
  CommonTools ${PTHREAD_LIB} fmt::fmt)

set(DEFINES)
if (ENABLE_VIXL_SIMULATOR)
  list(APPEND DEFINES -DVIXL_SIMULATOR=1)
endif()

add_executable(FEX
  FEXInterpreter.cpp
  AOT/AOTGenerator.cpp)

target_compile_definitions(FEX PRIVATE ${DEFINES})

# Enable FEX APIs to be used by targets that use target_link_libraries on FEX
set_target_properties(FEX PROPERTIES
  ENABLE_EXPORTS 1
  C_VISIBILITY_PRESET hidden
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN TRUE)

target_include_directories(FEX PRIVATE ${CMAKE_BINARY_DIR}/generated)

target_link_libraries(FEX PRIVATE ${LIBS})

target_compile_options(FEX PRIVATE ${FEX_TUNE_COMPILE_FLAGS})

set_target_properties(FEX PROPERTIES OUTPUT_NAME ${POWERARM_EXE_PREFIX})

LinkerGC(FEX)

install(TARGETS FEX RUNTIME
    DESTINATION bin
    COMPONENT Runtime)

# Create a copy of POWERarm with legacy names until phased out.
install(PROGRAMS ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${POWERARM_EXE_PREFIX}
  RENAME ${POWERARM_EXE_PREFIX}Interpreter
  DESTINATION bin
  COMPONENT LegacyRuntime)

if (ARCHITECTURE_arm64 OR ARCHITECTURE_ppc64le)
  # POWERarm registers AArch64 ELF only (Data/binfmts/POWERarm-aarch64.*).
  set(POWERARM_BINFMT_NAME "${POWERARM_EXE_PREFIX}-aarch64")
  if (NOT USE_LEGACY_BINFMTMISC)
    # Just restart the systemd service
    add_custom_target(binfmt_misc
      echo "Restarting systemd service now."
      COMMAND "service" "systemd-binfmt" "restart")
  else()
    # Check for conflicting binfmt before installing
    set(CONFLICTING_BINFMTS_64
      ${CMAKE_INSTALL_PREFIX}/share/binfmts/qemu-aarch64)

    find_program(UPDATE_BINFMTS_PROGRAM update-binfmts)
    if (UPDATE_BINFMTS_PROGRAM)
      add_custom_target(binfmt_misc
        echo "Attempting to install POWERarm binfmt_misc now."
        COMMAND "${CMAKE_SOURCE_DIR}/Scripts/CheckBinfmtNotInstall.sh" ${CONFLICTING_BINFMTS_64}
        COMMAND "update-binfmts" "--importdir=${CMAKE_INSTALL_PREFIX}/share/binfmts/" "--import" "${POWERARM_BINFMT_NAME}"
        COMMAND ${CMAKE_COMMAND} -E
        echo "POWERarm binfmt_misc installed")

      if(TARGET uninstall)
        add_custom_target(uninstall_binfmt_misc
          COMMAND update-binfmts --unimport ${POWERARM_BINFMT_NAME} || (exit 0))

        add_dependencies(uninstall uninstall_binfmt_misc)
      endif()
    else()
      # In the case of update-binfmts not being available (Arch for example) then we need to install manually
      add_custom_target(binfmt_misc
        COMMAND ${CMAKE_COMMAND} -E
          echo "Attempting to remove POWERarm misc prior to install. Ignore permission denied"
        COMMAND ${CMAKE_COMMAND} -E
          echo -1 > /proc/sys/fs/binfmt_misc/${POWERARM_BINFMT_NAME} || (exit 0)
        COMMAND ${CMAKE_COMMAND} -E
          echo "Attempting to install POWERarm misc now."
        COMMAND ${CMAKE_COMMAND} -E
          echo
          ':${POWERARM_BINFMT_NAME}:M:0:\\x7fELF\\x02\\x01\\x01\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x02\\x00\\xb7\\x00:\\xff\\xff\\xff\\xff\\xff\\xfe\\xfe\\x00\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xfe\\xff\\xff\\xff:${CMAKE_INSTALL_PREFIX}/bin/${POWERARM_EXE_PREFIX}:POCF' > /proc/sys/fs/binfmt_misc/register
        COMMAND ${CMAKE_COMMAND} -E
          echo "binfmt_misc POWERarm installed")

      if(TARGET uninstall)
        add_custom_target(uninstall_binfmt_misc
          COMMAND ${CMAKE_COMMAND} -E
            echo -1 > /proc/sys/fs/binfmt_misc/${POWERARM_BINFMT_NAME} || (exit 0))

        add_dependencies(uninstall uninstall_binfmt_misc)
      endif()
    endif()
  endif()
endif()
