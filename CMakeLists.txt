cmake_minimum_required(VERSION 3.20)

project(FabricEvolution
  VERSION 1.0.0
  DESCRIPTION "Vendor-neutral Summon Software Labs Fabric OS runtime for controlled control-plane evolution"
  HOMEPAGE_URL "https://github.com/summonlabs/Fabric-Evolution"
  LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(FABRIC_EVOLUTION_BUILD_TESTS "Build the Fabric Evolution test suite" ON)
option(FABRIC_EVOLUTION_BUILD_TOOLS "Build the command line tools and reference processes" ON)
option(FABRIC_EVOLUTION_BUILD_EXAMPLES "Build the examples" ON)
option(FABRIC_EVOLUTION_BUILD_BENCHMARKS "Build the benchmarks" ON)
option(FABRIC_EVOLUTION_WARNINGS_AS_ERRORS "Treat first-party warnings as errors" ON)
# AddressSanitizer is used where the toolchain ships the matching runtime. On a
# Visual Studio installation without the x64 AddressSanitizer component this
# option cannot link; the Debug configuration then provides the equivalent
# coverage through /RTC1 runtime checks, per-test debug-heap validation and leak
# reporting. See docs/PROOF.md for the exact proof boundary.
option(FABRIC_EVOLUTION_ENABLE_ASAN "Build with AddressSanitizer where the toolchain supports it" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---------------------------------------------------------------------------
# Warning configuration. First-party code builds with zero warnings.
# ---------------------------------------------------------------------------
# Applied to every first-party target (library, tools, tests, examples, benchmarks).
function(fabric_evolution_strict target)
  # Portable C stdio is used deliberately for the durability primitives; the
  # MSVC deprecation shim for fopen is not applicable to this code.
  target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8 /EHsc)
    if(FABRIC_EVOLUTION_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wcast-qual -Wunused
      -Woverloaded-virtual -Wold-style-cast -Wdouble-promotion -Wformat=2)
    if(FABRIC_EVOLUTION_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
  if(FABRIC_EVOLUTION_ENABLE_ASAN)
    if(MSVC)
      target_compile_options(${target} PRIVATE /fsanitize=address)
    else()
      target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=address)
    endif()
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
add_library(fabric_evolution STATIC
  src/strong_types.cpp
  src/digest.cpp
  src/json.cpp
  src/status.cpp
  src/versions.cpp
  src/features.cpp
  src/ids.cpp
  src/clock.cpp
  src/authority.cpp
  src/explain.cpp
  src/compatibility.cpp
  src/manifest.cpp
  src/protocol.cpp
  src/migration.cpp
  src/persistence.cpp
  src/framing.cpp
  src/transport.cpp
  src/handoff.cpp
  src/campaign.cpp
  src/epoch_log.cpp
  src/node.cpp
  src/controller.cpp
)
add_library(FabricEvolution::fabric_evolution ALIAS fabric_evolution)

target_include_directories(fabric_evolution
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
fabric_evolution_strict(fabric_evolution)
target_compile_features(fabric_evolution PUBLIC cxx_std_20)

if(WIN32)
  target_link_libraries(fabric_evolution PUBLIC ws2_32)
endif()

set_target_properties(fabric_evolution PROPERTIES
  OUTPUT_NAME fabric_evolution
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR})

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
install(TARGETS fabric_evolution
  EXPORT FabricEvolutionTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT FabricEvolutionTargets
  FILE FabricEvolutionTargets.cmake
  NAMESPACE FabricEvolution::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FabricEvolution)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/FabricEvolutionConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/FabricEvolutionConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FabricEvolution)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/FabricEvolutionConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/FabricEvolutionConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/FabricEvolutionConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FabricEvolution)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
  DESTINATION ${CMAKE_INSTALL_DATADIR}/FabricEvolution)

# ---------------------------------------------------------------------------
# Subdirectories
# ---------------------------------------------------------------------------
if(FABRIC_EVOLUTION_BUILD_TOOLS)
  add_subdirectory(tools)
endif()
if(FABRIC_EVOLUTION_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
# The multi-process harness is shared by the end-to-end tests and the benchmarks.
if(FABRIC_EVOLUTION_BUILD_TESTS OR FABRIC_EVOLUTION_BUILD_BENCHMARKS)
  add_subdirectory(tests/support)
endif()
if(FABRIC_EVOLUTION_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
if(FABRIC_EVOLUTION_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
