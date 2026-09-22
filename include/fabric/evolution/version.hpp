// Fabric Evolution — build identity.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

// Kept in sync with the CMake project version by the build system.
#define FABRIC_EVOLUTION_VERSION_MAJOR 1
#define FABRIC_EVOLUTION_VERSION_MINOR 0
#define FABRIC_EVOLUTION_VERSION_PATCH 0
#define FABRIC_EVOLUTION_VERSION_STRING "1.0.0"

namespace fabric::evolution {

// Identifies the runtime's own release, as opposed to the software version of a
// component under evolution.
[[nodiscard]] constexpr const char* runtime_version() noexcept { return FABRIC_EVOLUTION_VERSION_STRING; }

}  // namespace fabric::evolution
