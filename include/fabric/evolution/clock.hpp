// Fabric Evolution — time sources.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <chrono>
#include <cstdint>

namespace fabric::evolution {

// All wall-clock values in the domain are milliseconds since the Unix epoch.
// Leases and persisted records use this scale because they must survive a
// process restart; monotonic clocks do not.
using TimestampMs = std::uint64_t;

inline constexpr TimestampMs kNoDeadline = 0;

// Abstract time source. Production code uses SystemClock; tests and deterministic
// replays use ManualClock so that lease expiry is exercised without sleeping.
class Clock {
 public:
  Clock() = default;
  virtual ~Clock();
  Clock(const Clock&) = delete;
  Clock& operator=(const Clock&) = delete;

  [[nodiscard]] virtual TimestampMs now_ms() const = 0;
};

class SystemClock final : public Clock {
 public:
  [[nodiscard]] TimestampMs now_ms() const override;
};

class ManualClock final : public Clock {
 public:
  explicit ManualClock(TimestampMs start_ms = 1000) : now_(start_ms) {}

  [[nodiscard]] TimestampMs now_ms() const override { return now_; }
  void advance(TimestampMs delta_ms) { now_ += delta_ms; }
  void set(TimestampMs value) { now_ = value; }

 private:
  TimestampMs now_;
};

}  // namespace fabric::evolution
