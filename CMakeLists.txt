cmake_minimum_required(VERSION 3.21)

project(LoadShedding
  VERSION 1.0.0
  DESCRIPTION "Deterministic facility load-shedding authority for the Data Center Control Plane"
  HOMEPAGE_URL "https://github.com/summonlabs/Load-Shedding"
  LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(LOAD_SHEDDING_IS_TOP_LEVEL ON)
else()
  set(LOAD_SHEDDING_IS_TOP_LEVEL OFF)
endif()

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 20)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(LOAD_SHEDDING_IS_TOP_LEVEL)
  option(LOAD_SHEDDING_BUILD_TESTS "Build the Load Shedding test suite" ON)
  option(LOAD_SHEDDING_BUILD_EXAMPLES "Build the Load Shedding examples" ON)
  option(LOAD_SHEDDING_BUILD_BENCHMARKS "Build the Load Shedding benchmarks" ON)
  option(LOAD_SHEDDING_BUILD_TOOLS "Build the Load Shedding command line tool" ON)
else()
  option(LOAD_SHEDDING_BUILD_TESTS "Build the Load Shedding test suite" OFF)
  option(LOAD_SHEDDING_BUILD_EXAMPLES "Build the Load Shedding examples" OFF)
  option(LOAD_SHEDDING_BUILD_BENCHMARKS "Build the Load Shedding benchmarks" OFF)
  option(LOAD_SHEDDING_BUILD_TOOLS "Build the Load Shedding command line tool" OFF)
endif()

option(LOAD_SHEDDING_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" ON)
option(LOAD_SHEDDING_ENABLE_ASAN "Build the library and tests with AddressSanitizer" OFF)

# ---------------------------------------------------------------------------
# First-party compile options
# ---------------------------------------------------------------------------
function(load_shedding_apply_strict_options target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
    if(LOAD_SHEDDING_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Wshadow
      -Wconversion
      -Wsign-conversion
      -Wnon-virtual-dtor
      -Wcast-qual
      -Wdouble-promotion)
    if(LOAD_SHEDDING_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Generated version header
# ---------------------------------------------------------------------------
set(LOAD_SHEDDING_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/include/load_shedding/version.hpp.in"
  "${LOAD_SHEDDING_GENERATED_INCLUDE_DIR}/load_shedding/version.hpp"
  @ONLY)

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
add_library(load_shedding
  src/status.cpp
  src/text.cpp
  src/units.cpp
  src/ids.cpp
  src/digest.cpp
  src/json.cpp
  src/policy.cpp
  src/facility.cpp
  src/plan.cpp
  src/recovery.cpp
  src/effect.cpp
  src/requests.cpp
  src/state.cpp
  src/history.cpp
  src/store_inspect.cpp
  src/json_io.cpp
  src/engine.cpp
  src/detail/codec.cpp
  src/detail/crc32c.cpp
  src/detail/durable_store.cpp
  src/detail/file_lock.cpp
  src/detail/platform_io.cpp
  src/detail/planner.cpp
  src/detail/serialization.cpp)
add_library(LoadShedding::load_shedding ALIAS load_shedding)

target_include_directories(load_shedding
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${LOAD_SHEDDING_GENERATED_INCLUDE_DIR}>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_compile_features(load_shedding PUBLIC cxx_std_20)
load_shedding_apply_strict_options(load_shedding)

set_target_properties(load_shedding PROPERTIES
  OUTPUT_NAME "load_shedding"
  VERSION "${PROJECT_VERSION}"
  SOVERSION "${PROJECT_VERSION_MAJOR}"
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON)

if(LOAD_SHEDDING_ENABLE_ASAN AND MSVC)
  target_compile_options(load_shedding PUBLIC /fsanitize=address)
endif()

if(BUILD_SHARED_LIBS AND WIN32)
  set_target_properties(load_shedding PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
install(TARGETS load_shedding
  EXPORT LoadSheddingTargets
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/load_shedding"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  FILES_MATCHING PATTERN "*.hpp")
install(FILES "${LOAD_SHEDDING_GENERATED_INCLUDE_DIR}/load_shedding/version.hpp"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/load_shedding")

set(LOAD_SHEDDING_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/LoadShedding")

install(EXPORT LoadSheddingTargets
  NAMESPACE LoadShedding::
  FILE LoadSheddingTargets.cmake
  DESTINATION "${LOAD_SHEDDING_INSTALL_CMAKEDIR}")

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/LoadSheddingConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/LoadSheddingConfig.cmake"
  INSTALL_DESTINATION "${LOAD_SHEDDING_INSTALL_CMAKEDIR}")

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/LoadSheddingConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/LoadSheddingConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/LoadSheddingConfigVersion.cmake"
  DESTINATION "${LOAD_SHEDDING_INSTALL_CMAKEDIR}")

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE" "${CMAKE_CURRENT_SOURCE_DIR}/NOTICE"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/LoadShedding")

# ---------------------------------------------------------------------------
# Subprojects
# ---------------------------------------------------------------------------
if(LOAD_SHEDDING_IS_TOP_LEVEL AND (LOAD_SHEDDING_BUILD_TESTS OR LOAD_SHEDDING_BUILD_EXAMPLES))
  enable_testing()
endif()

if(LOAD_SHEDDING_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(LOAD_SHEDDING_BUILD_BENCHMARKS)
  add_subdirectory(bench)
endif()

if(LOAD_SHEDDING_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(LOAD_SHEDDING_BUILD_TESTS)
  add_subdirectory(tests)
endif()
