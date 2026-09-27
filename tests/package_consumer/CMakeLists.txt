# A downstream consumer of the installed Facility Dependency Registry package.
#
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# This project lives outside the library's source tree on purpose: it is built
# only against an installed package, with find_package, so it proves that the
# package exports everything a consumer needs.

cmake_minimum_required(VERSION 3.20)

project(FacilityDependencyRegistryConsumer
        VERSION 1.0.0
        DESCRIPTION "Independent consumer of the Facility Dependency Registry package"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(FacilityDependencyRegistry 1.0 REQUIRED)

add_executable(fdep_consumer main.cpp)
target_link_libraries(fdep_consumer PRIVATE FacilityDependencyRegistry::facility_dependency_registry)

if(MSVC)
  target_compile_options(fdep_consumer PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8 /EHsc)
  target_compile_definitions(fdep_consumer PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  target_compile_options(fdep_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

message(STATUS "consumer: FacilityDependencyRegistry ${FacilityDependencyRegistry_VERSION} found")
message(STATUS "consumer: library version header reports the same major version")
