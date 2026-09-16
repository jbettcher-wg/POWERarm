add_executable(FEXOfflineCompiler Main.cpp)

target_link_libraries(FEXOfflineCompiler PRIVATE
  Common
  CommonTools
  cpp-optparse
  FEXCore
  JemallocLibs
  LinuxEmulation
  ${PTHREAD_LIB}
  fmt::fmt)

set_target_properties(FEXOfflineCompiler PROPERTIES OUTPUT_NAME ${POWERARM_EXE_PREFIX}OfflineCompiler)

LinkerGC(FEXOfflineCompiler)

install(TARGETS FEXOfflineCompiler RUNTIME
  DESTINATION bin
  COMPONENT Runtime)
