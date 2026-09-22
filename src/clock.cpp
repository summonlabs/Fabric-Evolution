// Fabric Evolution — time sources (implementation).
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric/evolution/clock.hpp"

namespace fabric::evolution {

Clock::~Clock() = default;

TimestampMs SystemClock::now_ms() const {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<TimestampMs>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

}  // namespace fabric::evolution
