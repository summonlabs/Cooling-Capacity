# Cooling Capacity - build, test, install and packaging rules.
#
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# Everything here is target based: no directory-global flags are set, so the
# library, the tools, the examples, the benchmarks and the tests each carry
# exactly the options they need, and nothing this project uses for itself can
# leak into the installed package.

cmake_minimum_required(VERSION 3.20)

project(CoolingCapacity
        VERSION 1.0.0
        DESCRIPTION "Cooling-capacity accounting for the Data Center Control Plane"
        LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------

option(BUILD_SHARED_LIBS "Build cooling_capacity as a shared library instead of a static one" OFF)

option(CCAP_BUILD_TESTS "Build the Cooling Capacity test suite" ON)
option(CCAP_BUILD_TOOLS "Build the Cooling Capacity command line tool" ON)
option(CCAP_BUILD_EXAMPLES "Build the Cooling Capacity example programs" ON)
option(CCAP_BUILD_BENCHMARKS "Build the Cooling Capacity benchmarks" ON)
option(CCAP_WARNINGS_AS_ERRORS "Treat every compiler warning as an error" ON)
option(CCAP_SANITIZERS "Build with AddressSanitizer, and UndefinedBehaviorSanitizer on GCC and Clang" OFF)
option(CCAP_ANALYZE "Build with the MSVC static analyzer" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CheckCXXCompilerFlag)
include(CTest)

set(CCAP_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/CoolingCapacity"
    CACHE STRING "Install destination of the Cooling Capacity CMake package files")

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

# Applies the project build policy to one target. Every entry is PRIVATE, so
# none of it is part of the installed package.
function(ccap_configure_target target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8 /EHsc)
    if(CCAP_WARNINGS_AS_ERRORS AND NOT CCAP_ANALYZE)
      target_compile_options(${target} PRIVATE /WX)
    endif()
    target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(${target} PRIVATE $<$<CONFIG:Debug>:/RTC1>)
    target_compile_definitions(${target} PRIVATE $<$<CONFIG:Debug>:_ITERATOR_DEBUG_LEVEL=2>)
    target_compile_options(${target} PRIVATE
      $<$<CONFIG:Release>:/O2>
      $<$<CONFIG:Release>:/Gy>
      $<$<CONFIG:Release>:/Gw>)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    if(CCAP_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
  if(MSVC AND CCAP_ANALYZE)
    target_compile_options(${target} PRIVATE /analyze /analyze:WX-)
  endif()
  if(CCAP_SANITIZERS)
    target_compile_options(${target} PRIVATE ${CCAP_SANITIZER_COMPILE_OPTIONS})
    target_link_options(${target} PRIVATE ${CCAP_SANITIZER_LINK_OPTIONS})
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Sanitizers
# ---------------------------------------------------------------------------

if(CCAP_SANITIZERS)
  if(MSVC)
    check_cxx_compiler_flag("/fsanitize=address" CCAP_COMPILER_HAS_ASAN)
    if(NOT CCAP_COMPILER_HAS_ASAN)
      message(FATAL_ERROR
        "CCAP_SANITIZERS=ON cannot be used with ${CMAKE_CXX_COMPILER_ID} "
        "${CMAKE_CXX_COMPILER_VERSION}: a program built with /fsanitize=address does not "
        "compile and link here. Either the toolset predates AddressSanitizer support "
        "(Visual Studio 2019 16.9) or the optional C++ AddressSanitizer component is not "
        "installed. Reconfigure with -DCCAP_SANITIZERS=OFF or install that component.")
    endif()
    set(CCAP_SANITIZER_COMPILE_OPTIONS /fsanitize=address /Zi)
    set(CCAP_SANITIZER_LINK_OPTIONS /fsanitize=address)
    message(STATUS "CoolingCapacity: AddressSanitizer enabled (MSVC)")
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    check_cxx_compiler_flag("-fsanitize=address,undefined" CCAP_COMPILER_HAS_ASAN_UBSAN)
    if(NOT CCAP_COMPILER_HAS_ASAN_UBSAN)
      message(FATAL_ERROR
        "CCAP_SANITIZERS=ON cannot be used with ${CMAKE_CXX_COMPILER_ID} "
        "${CMAKE_CXX_COMPILER_VERSION}: the sanitizer runtime is unavailable. "
        "Reconfigure with -DCCAP_SANITIZERS=OFF.")
    endif()
    set(CCAP_SANITIZER_COMPILE_OPTIONS -fsanitize=address,undefined -fno-omit-frame-pointer)
    set(CCAP_SANITIZER_LINK_OPTIONS -fsanitize=address,undefined)
    message(STATUS "CoolingCapacity: AddressSanitizer and UndefinedBehaviorSanitizer enabled")
  else()
    message(FATAL_ERROR
      "CCAP_SANITIZERS=ON is supported with MSVC (/fsanitize=address) and with GCC or Clang "
      "(-fsanitize=address,undefined); the configured compiler is "
      "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}. "
      "Reconfigure with -DCCAP_SANITIZERS=OFF.")
  endif()
endif()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------

set(CCAP_PUBLIC_HEADERS
  include/cooling_capacity/accounting.hpp
  include/cooling_capacity/candidate.hpp
  include/cooling_capacity/canonical.hpp
  include/cooling_capacity/clock.hpp
  include/cooling_capacity/commitment.hpp
  include/cooling_capacity/cooling_capacity.hpp
  include/cooling_capacity/diff.hpp
  include/cooling_capacity/digest.hpp
  include/cooling_capacity/engine.hpp
  include/cooling_capacity/errors.hpp
  include/cooling_capacity/evidence.hpp
  include/cooling_capacity/explanation.hpp
  include/cooling_capacity/export.hpp
  include/cooling_capacity/ids.hpp
  include/cooling_capacity/limits.hpp
  include/cooling_capacity/medium.hpp
  include/cooling_capacity/model.hpp
  include/cooling_capacity/policy.hpp
  include/cooling_capacity/reconciliation.hpp
  include/cooling_capacity/rollup.hpp
  include/cooling_capacity/snapshot.hpp
  include/cooling_capacity/store.hpp
  include/cooling_capacity/text.hpp
  include/cooling_capacity/units.hpp
  include/cooling_capacity/version.hpp)

set(CCAP_SOURCES
  src/accounting.cpp
  src/candidate.cpp
  src/canonical.cpp
  src/clock.cpp
  src/commitment.cpp
  src/diff.cpp
  src/digest.cpp
  src/engine.cpp
  src/errors.cpp
  src/evidence.cpp
  src/explanation.cpp
  src/file_lock.cpp
  src/ids.cpp
  src/limits.cpp
  src/medium.cpp
  src/platform.cpp
  src/policy.cpp
  src/reconciliation.cpp
  src/rollup.cpp
  src/snapshot.cpp
  src/store.cpp
  src/text.cpp
  src/units.cpp
  src/validation.cpp
  src/version.cpp)

add_library(cooling_capacity ${CCAP_SOURCES})
add_library(CoolingCapacity::cooling_capacity ALIAS cooling_capacity)

target_include_directories(cooling_capacity
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(cooling_capacity PUBLIC cxx_std_20)
set_target_properties(cooling_capacity PROPERTIES
  OUTPUT_NAME cooling_capacity
  POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)

if(BUILD_SHARED_LIBS)
  target_compile_definitions(cooling_capacity
    PUBLIC CCAP_SHARED_USE
    PRIVATE CCAP_SHARED_BUILD)
endif()

ccap_configure_target(cooling_capacity)

# ---------------------------------------------------------------------------
# Tools, examples and benchmarks
# ---------------------------------------------------------------------------

function(ccap_add_console_target target)
  add_executable(${target} ${ARGN})
  target_link_libraries(${target} PRIVATE CoolingCapacity::cooling_capacity)
  ccap_configure_target(${target})
endfunction()

if(CCAP_BUILD_TOOLS)
  ccap_add_console_target(ccap tools/ccap_cli.cpp)
endif()

set(CCAP_EXAMPLE_SOURCES
  examples/01_zone_capacity.cpp
  examples/02_shared_plant.cpp
  examples/03_redundancy_and_derating.cpp
  examples/04_unknown_is_not_zero.cpp
  examples/05_candidate_load.cpp
  examples/06_commitments_and_preconditions.cpp
  examples/07_persistence_and_recovery.cpp
  examples/08_generation_diff.cpp)

if(CCAP_BUILD_EXAMPLES)
  foreach(source IN LISTS CCAP_EXAMPLE_SOURCES)
    string(SUBSTRING "${source}" 9 2 CCAP_EXAMPLE_NUMBER)
    ccap_add_console_target(ccap_example_${CCAP_EXAMPLE_NUMBER} ${source})
  endforeach()
endif()

if(CCAP_BUILD_BENCHMARKS)
  ccap_add_console_target(ccap_benchmark benchmarks/benchmark_capacity.cpp)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

set(CCAP_TEST_SOURCES
  tests/test_adversarial.cpp
  tests/test_authority.cpp
  tests/test_candidate.cpp
  tests/test_canonical.cpp
  tests/test_commitment.cpp
  tests/test_concurrency.cpp
  tests/test_corruption.cpp
  tests/test_crash.cpp
  tests/test_diff.cpp
  tests/test_errors.cpp
  tests/test_evidence.cpp
  tests/test_explanation.cpp
  tests/test_limits.cpp
  tests/test_medium.cpp
  tests/test_multiprocess.cpp
  tests/test_paths.cpp
  tests/test_persistence.cpp
  tests/test_policy.cpp
  tests/test_property.cpp
  tests/test_reconciliation.cpp
  tests/test_redundancy.cpp
  tests/test_restart.cpp
  tests/test_rollup.cpp
  tests/test_shared_plant.cpp
  tests/test_snapshot.cpp
  tests/test_units.cpp
  tests/test_validation_precedence.cpp
  tests/test_version.cpp)

if(CCAP_BUILD_TESTS)
  add_executable(ccap_tests
    ${CCAP_TEST_SOURCES}
    tests/support/test_harness.cpp
    tests/support/test_main.cpp
    tests/support/test_process.cpp
    tests/support/fixtures.cpp)
  target_link_libraries(ccap_tests PRIVATE CoolingCapacity::cooling_capacity)
  target_include_directories(ccap_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)
  ccap_configure_target(ccap_tests)

  # The test binary re-executes itself to obtain real independent operating
  # system processes for the writer-lock, restart and crash tests.
  add_test(NAME ccap.tests COMMAND ccap_tests)

  foreach(source IN LISTS CCAP_EXAMPLE_SOURCES)
    string(SUBSTRING "${source}" 9 2 CCAP_EXAMPLE_NUMBER)
    if(CCAP_BUILD_EXAMPLES)
      add_test(NAME ccap.example.${CCAP_EXAMPLE_NUMBER} COMMAND ccap_example_${CCAP_EXAMPLE_NUMBER})
    endif()
  endforeach()

  if(CCAP_BUILD_TOOLS)
    add_test(NAME ccap.cli.selfcheck COMMAND ccap ${CMAKE_CURRENT_BINARY_DIR}/cli-selfcheck --self-check)
    add_test(NAME ccap.cli.scenario COMMAND ccap ${CMAKE_CURRENT_BINARY_DIR}/cli-scenario --scenario)
  endif()

  if(CCAP_BUILD_BENCHMARKS)
    add_test(NAME ccap.benchmark.smoke COMMAND ccap_benchmark --quick)
  endif()
endif()

# ---------------------------------------------------------------------------
# Install and package
# ---------------------------------------------------------------------------

install(TARGETS cooling_capacity
  EXPORT CoolingCapacityTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(FILES ${CCAP_PUBLIC_HEADERS}
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/cooling_capacity)

if(CCAP_BUILD_TOOLS)
  install(TARGETS ccap RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(EXPORT CoolingCapacityTargets
  FILE CoolingCapacityTargets.cmake
  NAMESPACE CoolingCapacity::
  DESTINATION ${CCAP_INSTALL_CMAKEDIR})

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CoolingCapacityConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/CoolingCapacityConfig.cmake
  INSTALL_DESTINATION ${CCAP_INSTALL_CMAKEDIR})

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/CoolingCapacityConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/CoolingCapacityConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/CoolingCapacityConfigVersion.cmake
  DESTINATION ${CCAP_INSTALL_CMAKEDIR})

install(FILES LICENSE NOTICE DESTINATION ${CMAKE_INSTALL_DATADIR}/CoolingCapacity)

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

message(STATUS "CoolingCapacity ${PROJECT_VERSION}")
message(STATUS "  compiler            : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  build type          : ${CMAKE_BUILD_TYPE}")
message(STATUS "  warnings as errors  : ${CCAP_WARNINGS_AS_ERRORS}")
message(STATUS "  sanitizers          : ${CCAP_SANITIZERS}")
message(STATUS "  tests/tools/examples: ${CCAP_BUILD_TESTS}/${CCAP_BUILD_TOOLS}/${CCAP_BUILD_EXAMPLES}")
