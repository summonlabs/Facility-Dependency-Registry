# Facility Dependency Registry - build, test, install and packaging rules.
#
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# Everything here is target based: no directory-global flags are set, so the
# library, the tools, the examples, the benchmarks and the tests each carry
# exactly the options they need, and nothing this project uses for itself can
# leak into the installed package.

cmake_minimum_required(VERSION 3.20)

project(FacilityDependencyRegistry
        VERSION 1.0.0
        DESCRIPTION "Explicit facility dependency graph registry for the Data Center Control Plane"
        LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------

option(BUILD_SHARED_LIBS "Build facility_dependency_registry as a shared library instead of a static one" OFF)

option(FDEP_BUILD_TESTS "Build the Facility Dependency Registry test suite" ON)
option(FDEP_BUILD_TOOLS "Build the Facility Dependency Registry command line tools" ON)
option(FDEP_BUILD_EXAMPLES "Build the Facility Dependency Registry example programs" ON)
option(FDEP_BUILD_BENCHMARKS "Build the Facility Dependency Registry benchmarks" ON)
option(FDEP_WARNINGS_AS_ERRORS "Treat every compiler warning as an error" ON)
option(FDEP_SANITIZERS "Build with AddressSanitizer, and UndefinedBehaviorSanitizer on GCC and Clang" OFF)
option(FDEP_ANALYZE "Build with the MSVC static analyzer" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CheckCXXCompilerFlag)
include(CTest)

set(FDEP_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/FacilityDependencyRegistry"
    CACHE STRING "Install destination of the Facility Dependency Registry CMake package files")

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

# Applies the project build policy to one target. Every entry is PRIVATE, so
# none of it is part of the installed package.
function(fdep_configure_target target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8 /EHsc)
    if(FDEP_WARNINGS_AS_ERRORS AND NOT FDEP_ANALYZE)
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
    if(FDEP_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
  if(MSVC AND FDEP_ANALYZE)
    target_compile_options(${target} PRIVATE /analyze /analyze:WX-)
  endif()
  if(FDEP_SANITIZERS)
    target_compile_options(${target} PRIVATE ${FDEP_SANITIZER_COMPILE_OPTIONS})
    target_link_options(${target} PRIVATE ${FDEP_SANITIZER_LINK_OPTIONS})
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Sanitizers and static analysis
# ---------------------------------------------------------------------------

if(FDEP_SANITIZERS)
  if(MSVC)
    check_cxx_compiler_flag("/fsanitize=address" FDEP_COMPILER_HAS_ASAN)
    if(NOT FDEP_COMPILER_HAS_ASAN)
      message(FATAL_ERROR
        "FDEP_SANITIZERS=ON cannot be used with ${CMAKE_CXX_COMPILER_ID} "
        "${CMAKE_CXX_COMPILER_VERSION}: a program built with /fsanitize=address does not "
        "compile and link here. Either the toolset predates AddressSanitizer support "
        "(Visual Studio 2019 16.9) or the optional C++ AddressSanitizer component is not "
        "installed. Reconfigure with -DFDEP_SANITIZERS=OFF or install that component.")
    endif()
    set(FDEP_SANITIZER_COMPILE_OPTIONS /fsanitize=address /Zi)
    set(FDEP_SANITIZER_LINK_OPTIONS /fsanitize=address)
    message(STATUS "FacilityDependencyRegistry: AddressSanitizer enabled (MSVC)")
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    check_cxx_compiler_flag("-fsanitize=address,undefined" FDEP_COMPILER_HAS_ASAN_UBSAN)
    if(NOT FDEP_COMPILER_HAS_ASAN_UBSAN)
      message(FATAL_ERROR
        "FDEP_SANITIZERS=ON cannot be used with ${CMAKE_CXX_COMPILER_ID} "
        "${CMAKE_CXX_COMPILER_VERSION}: the sanitizer runtime is unavailable. "
        "Reconfigure with -DFDEP_SANITIZERS=OFF.")
    endif()
    set(FDEP_SANITIZER_COMPILE_OPTIONS -fsanitize=address,undefined -fno-omit-frame-pointer)
    set(FDEP_SANITIZER_LINK_OPTIONS -fsanitize=address,undefined)
    message(STATUS "FacilityDependencyRegistry: AddressSanitizer and UndefinedBehaviorSanitizer enabled")
  else()
    message(FATAL_ERROR
      "FDEP_SANITIZERS=ON is supported with MSVC (/fsanitize=address) and with GCC or Clang "
      "(-fsanitize=address,undefined); the configured compiler is "
      "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}. "
      "Reconfigure with -DFDEP_SANITIZERS=OFF.")
  endif()
endif()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------

set(FDEP_PUBLIC_HEADERS
  include/facility_dependency_registry/cancel.hpp
  include/facility_dependency_registry/clock.hpp
  include/facility_dependency_registry/constraint.hpp
  include/facility_dependency_registry/digest.hpp
  include/facility_dependency_registry/edge.hpp
  include/facility_dependency_registry/errors.hpp
  include/facility_dependency_registry/export.hpp
  include/facility_dependency_registry/facility_dependency_registry.hpp
  include/facility_dependency_registry/ids.hpp
  include/facility_dependency_registry/lifecycle.hpp
  include/facility_dependency_registry/limits.hpp
  include/facility_dependency_registry/mask.hpp
  include/facility_dependency_registry/node_ref.hpp
  include/facility_dependency_registry/persistence.hpp
  include/facility_dependency_registry/provenance.hpp
  include/facility_dependency_registry/query.hpp
  include/facility_dependency_registry/registry.hpp
  include/facility_dependency_registry/requests.hpp
  include/facility_dependency_registry/snapshot.hpp
  include/facility_dependency_registry/version.hpp)

set(FDEP_SOURCES
  src/byte_codec.cpp
  src/canonical_codec.cpp
  src/clock.cpp
  src/constraint.cpp
  src/crc32.cpp
  src/digest.cpp
  src/durable_store.cpp
  src/edge.cpp
  src/errors.cpp
  src/export.cpp
  src/file_lock.cpp
  src/file_ops.cpp
  src/graph_algorithms.cpp
  src/graph_state.cpp
  src/ids.cpp
  src/json_export.cpp
  src/lifecycle.cpp
  src/limits.cpp
  src/node_ref.cpp
  src/provenance.cpp
  src/query.cpp
  src/registry.cpp
  src/registry_internal.cpp
  src/registry_mutations.cpp
  src/sha256.cpp
  src/snapshot.cpp
  src/utf8.cpp
  src/version.cpp)

add_library(facility_dependency_registry ${FDEP_SOURCES})
add_library(FacilityDependencyRegistry::facility_dependency_registry ALIAS facility_dependency_registry)

target_include_directories(facility_dependency_registry
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(facility_dependency_registry PUBLIC cxx_std_20)
set_target_properties(facility_dependency_registry PROPERTIES
  OUTPUT_NAME facility_dependency_registry
  POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)

if(WIN32)
  target_compile_definitions(facility_dependency_registry PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

fdep_configure_target(facility_dependency_registry)

# ---------------------------------------------------------------------------
# Tools, examples and benchmarks
# ---------------------------------------------------------------------------

function(fdep_add_console_target target)
  add_executable(${target} ${ARGN})
  target_link_libraries(${target} PRIVATE FacilityDependencyRegistry::facility_dependency_registry)
  fdep_configure_target(${target})
endfunction()

if(FDEP_BUILD_TOOLS)
  fdep_add_console_target(fdep tools/fdep_cli.cpp)
endif()

set(FDEP_EXAMPLE_SOURCES
  examples/01_declare_and_query.cpp
  examples/02_impact_cone.cpp
  examples/03_legal_cycles.cpp
  examples/04_stale_generation.cpp
  examples/05_persistence_and_recovery.cpp
  examples/06_unresolved_references.cpp
  examples/07_bounded_traversal.cpp
  examples/08_generation_diff.cpp)

if(FDEP_BUILD_EXAMPLES)
  set(FDEP_EXAMPLE_INDEX 0)
  foreach(source IN LISTS FDEP_EXAMPLE_SOURCES)
    math(EXPR FDEP_EXAMPLE_INDEX "${FDEP_EXAMPLE_INDEX} + 1")
    string(SUBSTRING "${source}" 9 2 FDEP_EXAMPLE_NUMBER)
    set(FDEP_EXAMPLE_TARGET "fdep_example_${FDEP_EXAMPLE_NUMBER}")
    fdep_add_console_target(${FDEP_EXAMPLE_TARGET} ${source})
  endforeach()
endif()

if(FDEP_BUILD_BENCHMARKS)
  fdep_add_console_target(fdep_benchmark benchmarks/benchmark_registry.cpp)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

set(FDEP_TEST_SOURCES
  tests/test_adversarial.cpp
  tests/test_authority.cpp
  tests/test_cancellation.cpp
  tests/test_codec.cpp
  tests/test_concurrency.cpp
  tests/test_constraint.cpp
  tests/test_corruption.cpp
  tests/test_crash.cpp
  tests/test_cycles.cpp
  tests/test_digest.cpp
  tests/test_diff.cpp
  tests/test_duplicate_semantics.cpp
  tests/test_export.cpp
  tests/test_external_refs.cpp
  tests/test_ids.cpp
  tests/test_impact.cpp
  tests/test_lifecycle.cpp
  tests/test_limits.cpp
  tests/test_multiprocess.cpp
  tests/test_node_ref.cpp
  tests/test_paths.cpp
  tests/test_persistence.cpp
  tests/test_property.cpp
  tests/test_provenance.cpp
  tests/test_query.cpp
  tests/test_recovery.cpp
  tests/test_registry_mutation.cpp
  tests/test_scc.cpp
  tests/test_snapshot.cpp
  tests/test_traversal.cpp
  tests/test_version.cpp)

if(FDEP_BUILD_TESTS)
  add_executable(fdep_tests
    ${FDEP_TEST_SOURCES}
    tests/support/test_harness.cpp
    tests/support/test_main.cpp
    tests/support/test_process.cpp)
  target_link_libraries(fdep_tests PRIVATE FacilityDependencyRegistry::facility_dependency_registry)
  target_include_directories(fdep_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)
  fdep_configure_target(fdep_tests)

  # The test binary re-executes itself to obtain real independent operating
  # system processes for the shared-writer, restart and crash tests.
  add_test(NAME fdep.tests COMMAND fdep_tests)

  foreach(source IN LISTS FDEP_EXAMPLE_SOURCES)
    string(SUBSTRING "${source}" 9 2 FDEP_EXAMPLE_NUMBER)
    if(FDEP_BUILD_EXAMPLES)
      add_test(NAME fdep.example.${FDEP_EXAMPLE_NUMBER}
               COMMAND fdep_example_${FDEP_EXAMPLE_NUMBER})
    endif()
  endforeach()

  if(FDEP_BUILD_TOOLS)
    add_test(NAME fdep.cli.selfcheck COMMAND fdep ${CMAKE_CURRENT_BINARY_DIR}/cli-selfcheck --self-check)
    add_test(NAME fdep.cli.scenario COMMAND fdep ${CMAKE_CURRENT_BINARY_DIR}/cli-scenario --scenario)
  endif()

  if(FDEP_BUILD_BENCHMARKS)
    add_test(NAME fdep.benchmark.smoke COMMAND fdep_benchmark --quick)
  endif()
endif()

# ---------------------------------------------------------------------------
# Install and package
# ---------------------------------------------------------------------------

install(TARGETS facility_dependency_registry
  EXPORT FacilityDependencyRegistryTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(FILES ${FDEP_PUBLIC_HEADERS}
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/facility_dependency_registry)

if(FDEP_BUILD_TOOLS)
  install(TARGETS fdep RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(EXPORT FacilityDependencyRegistryTargets
  FILE FacilityDependencyRegistryTargets.cmake
  NAMESPACE FacilityDependencyRegistry::
  DESTINATION ${FDEP_INSTALL_CMAKEDIR})

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/FacilityDependencyRegistryConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/FacilityDependencyRegistryConfig.cmake
  INSTALL_DESTINATION ${FDEP_INSTALL_CMAKEDIR})

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/FacilityDependencyRegistryConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/FacilityDependencyRegistryConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/FacilityDependencyRegistryConfigVersion.cmake
  DESTINATION ${FDEP_INSTALL_CMAKEDIR})

install(FILES LICENSE NOTICE DESTINATION ${CMAKE_INSTALL_DATADIR}/FacilityDependencyRegistry)

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

message(STATUS "FacilityDependencyRegistry ${PROJECT_VERSION}")
message(STATUS "  compiler            : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  build type          : ${CMAKE_BUILD_TYPE}")
message(STATUS "  warnings as errors  : ${FDEP_WARNINGS_AS_ERRORS}")
message(STATUS "  sanitizers          : ${FDEP_SANITIZERS}")
message(STATUS "  tests/tools/examples: ${FDEP_BUILD_TESTS}/${FDEP_BUILD_TOOLS}/${FDEP_BUILD_EXAMPLES}")
