function(GenBinFmt Name)
  # Get the filename only component
  get_filename_component(FMT_NAME ${Name} NAME_WE)

  # Configure it
  configure_file(${Name} ${CMAKE_BINARY_DIR}/Data/binfmts/${FMT_NAME})

  # Then install the configured binfmt
  install(FILES ${CMAKE_BINARY_DIR}/Data/binfmts/${FMT_NAME}
    DESTINATION ${CMAKE_INSTALL_PREFIX}/share/binfmts/
    COMPONENT Runtime)
endfunction()

# POWERarm registers AArch64 ELF only. The x86/x86-64 entries belong to
# fastppcx86 and must not be installed from this tree.
if (NOT USE_LEGACY_BINFMTMISC)
  configure_file(POWERarm-aarch64.conf.in ${CMAKE_BINARY_DIR}/Data/binfmts/POWERarm-aarch64.conf)

  install(FILES ${CMAKE_BINARY_DIR}/Data/binfmts/POWERarm-aarch64.conf
    DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/binfmt.d/
    COMPONENT Runtime)
else()
  GenBinFmt(POWERarm-aarch64.in)
endif()
