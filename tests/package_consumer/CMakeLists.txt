# Cooling Capacity package consumer.
#
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# An independent out-of-tree project that consumes the installed package. It
# configures with find_package(CoolingCapacity 1.0 REQUIRED), links the imported
# target CoolingCapacity::cooling_capacity and runs a minimal lifecycle:
#
#   cmake -S tests/package_consumer -B build/package-consumer \
#         -DCMAKE_PREFIX_PATH=<install prefix>
#   cmake --build build/package-consumer
#   ctest --test-dir build/package-consumer

cmake_minimum_required(VERSION 3.20)

project(CoolingCapacityPackageConsumer
        VERSION 1.0.0
        DESCRIPTION "Out-of-tree consumer of the Cooling Capacity package"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The package brings its own include directories and compile features through
# the imported target, so nothing about the library is repeated here.
find_package(CoolingCapacity 1.0 REQUIRED)

add_executable(ccap_package_consumer consumer.cpp)
target_link_libraries(ccap_package_consumer PRIVATE CoolingCapacity::cooling_capacity)

# The consumer keeps the library's warning level but decides its own policy on
# warnings as errors.
if(MSVC)
  target_compile_options(ccap_package_consumer PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8 /EHsc)
  target_compile_definitions(ccap_package_consumer PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  target_compile_options(ccap_package_consumer PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_test(NAME ccap.package.consumer COMMAND ccap_package_consumer)
