if(BUILD_TESTING)
  add_subdirectory(common)
  add_subdirectory(gate)
endif()

if(WIMI_BUILD_UNIT_TESTS)
  add_subdirectory(file)
endif()
