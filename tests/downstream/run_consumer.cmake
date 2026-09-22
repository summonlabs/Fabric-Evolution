# Fabric Evolution — downstream package consumer validation.
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# Installs the built tree into a scratch prefix, configures the independent
# consumer project against that prefix, builds it and runs it. A failure at any
# step fails the CTest case.

foreach(required IN ITEMS FABRIC_EVOLUTION_SOURCE_DIR FABRIC_EVOLUTION_BUILD_DIR CMAKE_COMMAND)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} must be defined")
  endif()
endforeach()

set(prefix "${FABRIC_EVOLUTION_BUILD_DIR}/package-validation/prefix")
set(consumer_build "${FABRIC_EVOLUTION_BUILD_DIR}/package-validation/consumer-build")
file(REMOVE_RECURSE "${FABRIC_EVOLUTION_BUILD_DIR}/package-validation")
file(MAKE_DIRECTORY "${prefix}")

message(STATUS "installing into ${prefix}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${FABRIC_EVOLUTION_BUILD_DIR}" --prefix "${prefix}"
          --config "${FABRIC_EVOLUTION_BUILD_TYPE}"
  RESULT_VARIABLE install_result
  OUTPUT_VARIABLE install_output
  ERROR_VARIABLE install_error)
if(NOT install_result EQUAL 0)
  message(FATAL_ERROR "install failed (${install_result})\n${install_output}\n${install_error}")
endif()

set(configure_command "${CMAKE_COMMAND}" -S "${FABRIC_EVOLUTION_SOURCE_DIR}/tests/downstream"
    -B "${consumer_build}" "-DCMAKE_PREFIX_PATH=${prefix}")
if(DEFINED FABRIC_EVOLUTION_GENERATOR AND NOT FABRIC_EVOLUTION_GENERATOR STREQUAL "")
  list(APPEND configure_command -G "${FABRIC_EVOLUTION_GENERATOR}")
endif()
if(DEFINED FABRIC_EVOLUTION_BUILD_TYPE AND NOT FABRIC_EVOLUTION_BUILD_TYPE STREQUAL "")
  list(APPEND configure_command "-DCMAKE_BUILD_TYPE=${FABRIC_EVOLUTION_BUILD_TYPE}")
endif()
if(DEFINED FABRIC_EVOLUTION_CXX_COMPILER AND NOT FABRIC_EVOLUTION_CXX_COMPILER STREQUAL "")
  list(APPEND configure_command "-DCMAKE_CXX_COMPILER=${FABRIC_EVOLUTION_CXX_COMPILER}")
endif()

message(STATUS "configuring the downstream consumer")
execute_process(
  COMMAND ${configure_command}
  RESULT_VARIABLE configure_result
  OUTPUT_VARIABLE configure_output
  ERROR_VARIABLE configure_error)
if(NOT configure_result EQUAL 0)
  message(FATAL_ERROR "downstream configure failed (${configure_result})\n${configure_output}\n${configure_error}")
endif()

message(STATUS "building the downstream consumer")
execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${consumer_build}" --config "${FABRIC_EVOLUTION_BUILD_TYPE}"
  RESULT_VARIABLE build_result
  OUTPUT_VARIABLE build_output
  ERROR_VARIABLE build_error)
if(NOT build_result EQUAL 0)
  message(FATAL_ERROR "downstream build failed (${build_result})\n${build_output}\n${build_error}")
endif()

message(STATUS "running the downstream consumer")
execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${consumer_build}" --target test --config "${FABRIC_EVOLUTION_BUILD_TYPE}"
  RESULT_VARIABLE test_result
  OUTPUT_VARIABLE test_output
  ERROR_VARIABLE test_error)
if(NOT test_result EQUAL 0)
  message(FATAL_ERROR "downstream test failed (${test_result})\n${test_output}\n${test_error}")
endif()

message(STATUS "downstream package validation succeeded")
message(STATUS "${test_output}")
