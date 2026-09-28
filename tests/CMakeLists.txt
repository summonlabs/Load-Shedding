add_library(load_shedding_test_support STATIC
  support/test_harness.cpp
  support/fixture.cpp
  support/proc.cpp)
target_link_libraries(load_shedding_test_support PUBLIC load_shedding)
target_include_directories(load_shedding_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/support")
load_shedding_apply_strict_options(load_shedding_test_support)

# The helper is a real executable: multiprocess and crash tests need a process
# boundary, not a thread boundary.
add_executable(load_shedding_test_helper support/helper_main.cpp)
target_link_libraries(load_shedding_test_helper PRIVATE load_shedding load_shedding_test_support)
load_shedding_apply_strict_options(load_shedding_test_helper)

set(LOAD_SHEDDING_TEST_SOURCES
  test_core.cpp
  test_policy.cpp
  test_planning.cpp
  test_determinism.cpp
  test_authority.cpp
  test_store.cpp
  test_crash.cpp
  test_recovery.cpp
  test_property.cpp
  test_concurrency.cpp
  test_cli.cpp
  test_hardening.cpp)

add_executable(load_shedding_tests ${LOAD_SHEDDING_TEST_SOURCES})
target_link_libraries(load_shedding_tests PRIVATE load_shedding load_shedding_test_support)
load_shedding_apply_strict_options(load_shedding_tests)

if(LOAD_SHEDDING_ENABLE_ASAN AND MSVC)
  target_compile_options(load_shedding_tests PRIVATE /fsanitize=address)
  target_compile_options(load_shedding_test_helper PRIVATE /fsanitize=address)
endif()

set(LOAD_SHEDDING_TEST_SCRATCH "${CMAKE_CURRENT_BINARY_DIR}/scratch")

set(LOAD_SHEDDING_TEST_ENVIRONMENT
  "LOAD_SHEDDING_TEST_SCRATCH=${LOAD_SHEDDING_TEST_SCRATCH}"
  "LOAD_SHEDDING_HELPER_EXE=$<TARGET_FILE:load_shedding_test_helper>")
if(LOAD_SHEDDING_BUILD_TOOLS)
  list(APPEND LOAD_SHEDDING_TEST_ENVIRONMENT
    "LOAD_SHEDDING_CLI_EXE=$<TARGET_FILE:load_shedding_cli>")
endif()

# One test entry per suite so that a failure names the proof obligation group it
# belongs to. No test carries a timeout: a hang is a defect, not a bound.
set(LOAD_SHEDDING_SUITES
  core policy planning determinism authority store crash recovery property concurrency cli
  hardening)
foreach(suite IN LISTS LOAD_SHEDDING_SUITES)
  add_test(NAME suite_${suite} COMMAND load_shedding_tests --filter ${suite}.)
  set_tests_properties(suite_${suite} PROPERTIES
    ENVIRONMENT "${LOAD_SHEDDING_TEST_ENVIRONMENT}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endforeach()

add_test(NAME install_downstream
  COMMAND ${CMAKE_COMMAND}
    -DLS_SOURCE_DIR=${CMAKE_SOURCE_DIR}
    -DLS_BINARY_DIR=${CMAKE_BINARY_DIR}
    -DLS_CONFIG=$<CONFIG>
    -DLS_GENERATOR=${CMAKE_GENERATOR}
    -DLS_PREFIX=${CMAKE_BINARY_DIR}/install-prefix
    -P "${CMAKE_SOURCE_DIR}/cmake/RunDownstreamCheck.cmake")
set_tests_properties(install_downstream PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
