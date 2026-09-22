# Fabric Evolution — shared multi-process test support.
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# The end-to-end tests and the benchmarks both drive the shipped tools as
# independent OS processes, so the harness that starts and stops them lives in
# its own library.

add_library(fabric_evolution_test_support STATIC process.cpp cluster.cpp)
target_link_libraries(fabric_evolution_test_support PUBLIC fabric_evolution)
target_include_directories(fabric_evolution_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
fabric_evolution_strict(fabric_evolution_test_support)

add_dependencies(fabric_evolution_test_support
  fabric-evolution fabric-evolution-node fabric-evolution-controller)

target_compile_definitions(fabric_evolution_test_support PUBLIC
  FABRIC_EVOLUTION_NODE_BINARY="$<TARGET_FILE:fabric-evolution-node>"
  FABRIC_EVOLUTION_CONTROLLER_BINARY="$<TARGET_FILE:fabric-evolution-controller>"
  FABRIC_EVOLUTION_CLI_BINARY="$<TARGET_FILE:fabric-evolution>")
