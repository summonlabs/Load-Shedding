set(LOAD_SHEDDING_EXAMPLES
  lifecycle
  protected_and_unknown
  stale_authority
  persistence_recovery)

foreach(example IN LISTS LOAD_SHEDDING_EXAMPLES)
  add_executable(example_${example} ${example}.cpp)
  target_link_libraries(example_${example} PRIVATE load_shedding)
  load_shedding_apply_strict_options(example_${example})
  add_test(NAME example_${example} COMMAND example_${example})
  set_tests_properties(example_${example} PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endforeach()
