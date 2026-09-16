add_executable(FEXBash FEXBash.cpp)

target_link_libraries(FEXBash PRIVATE FEXCore Common JemallocLibs)

set_target_properties(FEXBash PROPERTIES OUTPUT_NAME ${POWERARM_EXE_PREFIX}Bash)

LinkerGC(FEXBash)

install(TARGETS FEXBash RUNTIME
  DESTINATION bin
  COMPONENT Runtime)
