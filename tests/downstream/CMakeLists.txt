# Fabric Evolution — downstream consumer project.
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# Configure this project with CMAKE_PREFIX_PATH pointing at a Fabric Evolution
# installation. It is intentionally independent of the upstream build tree.

cmake_minimum_required(VERSION 3.20)
project(FabricEvolutionDownstream LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(FabricEvolution 1.0 REQUIRED CONFIG)

message(STATUS "FabricEvolution_VERSION=${FabricEvolution_VERSION}")
message(STATUS "FabricEvolution_DIR=${FabricEvolution_DIR}")

add_executable(fabric_evolution_downstream main.cpp)
target_link_libraries(fabric_evolution_downstream PRIVATE FabricEvolution::fabric_evolution)
if(MSVC)
  target_compile_options(fabric_evolution_downstream PRIVATE /W4 /WX)
else()
  target_compile_options(fabric_evolution_downstream PRIVATE -Wall -Wextra -Werror)
endif()

enable_testing()
add_test(NAME downstream_runs COMMAND fabric_evolution_downstream)
