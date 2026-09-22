# Fabric Evolution — command line tools and reference processes.
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.

add_library(fabric_evolution_tools_args STATIC args.cpp)
target_link_libraries(fabric_evolution_tools_args PUBLIC fabric_evolution)
target_include_directories(fabric_evolution_tools_args PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
fabric_evolution_strict(fabric_evolution_tools_args)

add_executable(fabric-evolution fabric-evolution.cpp)
target_link_libraries(fabric-evolution PRIVATE fabric_evolution_tools_args)
fabric_evolution_strict(fabric-evolution)

add_executable(fabric-evolution-node fabric-node.cpp)
target_link_libraries(fabric-evolution-node PRIVATE fabric_evolution_tools_args)
fabric_evolution_strict(fabric-evolution-node)

add_executable(fabric-evolution-controller fabric-controller.cpp)
target_link_libraries(fabric-evolution-controller PRIVATE fabric_evolution_tools_args)
fabric_evolution_strict(fabric-evolution-controller)

install(TARGETS fabric-evolution fabric-evolution-node fabric-evolution-controller
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
