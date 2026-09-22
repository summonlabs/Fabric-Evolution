# Fabric Evolution — test suite.
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.

set(FABRIC_EVOLUTION_TEST_SOURCES
  test_main.cpp
  test_primitives.cpp
  test_authority.cpp
  test_manifest.cpp
  test_migration.cpp
  test_protocol.cpp
  test_persistence.cpp
  test_node_process.cpp
  test_e2e_evolution.cpp
  test_e2e_faults.cpp
  test_e2e_cli.cpp
  test_property.cpp
  test_concurrency.cpp
  test_examples.cpp
)

add_executable(fabric_evolution_tests ${FABRIC_EVOLUTION_TEST_SOURCES})
target_link_libraries(fabric_evolution_tests PRIVATE fabric_evolution fabric_evolution_test_support)
fabric_evolution_strict(fabric_evolution_tests)
target_include_directories(fabric_evolution_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(fabric_evolution_tests PRIVATE
  FABRIC_EVOLUTION_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

add_test(NAME fabric_evolution_tests COMMAND fabric_evolution_tests)

# Deterministic property and seeded randomized suites are re-run several times so
# that seed-dependent failures surface during a normal test run.
add_test(NAME fabric_evolution_tests_property COMMAND fabric_evolution_tests --filter=property --repeat=3)
add_test(NAME fabric_evolution_tests_concurrency COMMAND fabric_evolution_tests --filter=concurrency)
add_test(NAME fabric_evolution_tests_e2e COMMAND fabric_evolution_tests --filter=e2e)

# The package consumer configures, builds and runs an independent downstream
# project against the installed artifacts.
add_test(NAME fabric_evolution_package_consumer
  COMMAND ${CMAKE_COMMAND}
    -DFABRIC_EVOLUTION_SOURCE_DIR=${CMAKE_SOURCE_DIR}
    -DFABRIC_EVOLUTION_BUILD_DIR=${CMAKE_BINARY_DIR}
    -DFABRIC_EVOLUTION_GENERATOR=${CMAKE_GENERATOR}
    -DFABRIC_EVOLUTION_BUILD_TYPE=${CMAKE_BUILD_TYPE}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/downstream/run_consumer.cmake)
