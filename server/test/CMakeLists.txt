if(BUILD_TESTING)
  add_subdirectory(common)
  add_subdirectory(gate)
  add_subdirectory(gateway)
endif()

if(IMM_BUILD_UNIT_TESTS)
  add_subdirectory(file)
endif()
