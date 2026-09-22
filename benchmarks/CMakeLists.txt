# Fabric Evolution — benchmarks.
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.

add_executable(fabric-evolution-benchmark evolution_benchmark.cpp)
target_link_libraries(fabric-evolution-benchmark PRIVATE fabric_evolution fabric_evolution_test_support)
target_include_directories(fabric-evolution-benchmark PRIVATE
  ${CMAKE_SOURCE_DIR}/tests
  ${CMAKE_SOURCE_DIR}/tests/support)
fabric_evolution_strict(fabric-evolution-benchmark)
