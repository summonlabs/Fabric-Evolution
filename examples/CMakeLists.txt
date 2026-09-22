# Fabric Evolution — examples.
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.

add_executable(fabric-evolution-example-plan plan_and_migrate.cpp)
target_link_libraries(fabric-evolution-example-plan PRIVATE fabric_evolution)
fabric_evolution_strict(fabric-evolution-example-plan)

add_executable(fabric-evolution-example-inspect inspect_controller.cpp)
target_link_libraries(fabric-evolution-example-inspect PRIVATE fabric_evolution)
fabric_evolution_strict(fabric-evolution-example-inspect)
