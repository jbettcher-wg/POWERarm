add_executable(FEXpidof pidof.cpp)

target_link_libraries(FEXpidof PRIVATE
  cpp-optparse
  JemallocDummy
  fmt::fmt
  range-v3::range-v3)

set_target_properties(FEXpidof PROPERTIES OUTPUT_NAME ${POWERARM_EXE_PREFIX}pidof)

LinkerGC(FEXpidof)

install(TARGETS FEXpidof RUNTIME
  DESTINATION bin
  COMPONENT Runtime)
