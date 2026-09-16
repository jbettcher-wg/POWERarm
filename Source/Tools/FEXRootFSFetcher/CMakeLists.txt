add_executable(FEXRootFSFetcher Main.cpp XXFileHash.cpp)
list(APPEND LIBS FEXCore Common JemallocDummy xxHash::xxhash ${PTHREAD_LIB})

set_target_properties(FEXRootFSFetcher PROPERTIES OUTPUT_NAME ${POWERARM_EXE_PREFIX}RootFSFetcher)

LinkerGC(FEXRootFSFetcher)

install(TARGETS FEXRootFSFetcher RUNTIME
  DESTINATION bin
  COMPONENT Runtime)

target_link_libraries(FEXRootFSFetcher PRIVATE ${LIBS})
