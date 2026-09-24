add_executable(FEXRootFSFetcher Main.cpp)
list(APPEND LIBS FEXCore Common JemallocDummy ${PTHREAD_LIB})

set_target_properties(FEXRootFSFetcher PROPERTIES OUTPUT_NAME ${POWERARM_EXE_PREFIX}RootFSFetcher)

LinkerGC(FEXRootFSFetcher)

install(TARGETS FEXRootFSFetcher RUNTIME
  DESTINATION bin
  COMPONENT Runtime)

# The tool drives Scripts/powerarm/rootfs/alarm_sysroot.py rather than reimplementing the
# pins, the signature check and the rootless extraction, so the scripts have to travel with
# it. POWERARM_SOURCE_DIR is the last-resort fallback for a binary run straight out of a
# build tree that was moved.
target_compile_definitions(FEXRootFSFetcher PRIVATE POWERARM_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

install(DIRECTORY ${PROJECT_SOURCE_DIR}/Scripts/powerarm/rootfs/
  DESTINATION share/${POWERARM_DIR_NAME}/rootfs
  COMPONENT Runtime
  FILES_MATCHING
    PATTERN "*.py"
    PATTERN "*.sh"
    PATTERN "*.manifest")

target_link_libraries(FEXRootFSFetcher PRIVATE ${LIBS})
