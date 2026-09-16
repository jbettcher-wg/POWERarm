if (NOT MINGW)
  add_subdirectory(APITests/)
  add_subdirectory(Utilities/)

  if (BUILD_THUNKS)
    add_subdirectory(ThunkLibs)
  endif()
endif()
