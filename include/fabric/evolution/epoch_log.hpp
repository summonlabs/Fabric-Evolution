// Fabric Evolution — durable epoch journal.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every authority-relevant decision is appended to a durable journal before it
// is acknowledged: authority grants, fence requests and acknowledgements, epoch
// advances, handoff phase transitions, migrations and reconciliations. The log is
// bounded: when it grows past the configured ceiling it is compacted by
// rewriting only the retained tail, using the same atomic-replace path as every
// other durable write. A damaged record stops the replay at that point instead
// of being interpreted.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "fabric/evolution/clock.hpp"
#include "fabric/evolution/ids.hpp"

namespace fabric::evolution {

enum class EpochEventKind : std::uint8_t {
  ShardEstablished = 0,
  LeaseGranted = 1,
  LeaseRenewed = 2,
  FenceRequested = 3,
  FenceAcknowledged = 4,
  LeaseRevoked = 5,
  EpochAdvanced = 6,
  HandoffPhase = 7,
  CampaignTransition = 8,
  MigrationApplied = 9,
  Reconciliation = 10,
  Refusal = 11,
};

[[nodiscard]] std::string_view to_string(EpochEventKind kind) noexcept;
[[nodiscard]] std::optional<EpochEventKind> epoch_event_kind_from(std::string_view text);

struct EpochEvent {
  EpochEventKind kind = EpochEventKind::Reconciliation;
  ShardId shard;
  EpochNumber epoch;
  Generation generation;
  TimestampMs at_ms = 0;
  Json payload = Json::object();
};

inline constexpr std::size_t kDefaultEpochLogCapacity = 4096;

class EpochLog {
 public:
  EpochLog(std::string path, std::size_t capacity = kDefaultEpochLogCapacity);

  Status append(const EpochEvent& event);
  // Replays the durable journal. Reports how many records were read and whether
  // the tail was damaged; a damaged tail is never silently ignored.
  Status replay();
  Status compact();

  [[nodiscard]] const std::vector<EpochEvent>& events() const noexcept { return events_; }
  [[nodiscard]] std::size_t size() const noexcept { return events_.size(); }
  [[nodiscard]] std::uint64_t replayed_records() const noexcept { return replayed_records_; }
  [[nodiscard]] bool tail_damaged() const noexcept { return tail_damaged_; }
  [[nodiscard]] std::string diagnostic() const { return diagnostic_; }
  [[nodiscard]] const std::string& path() const noexcept { return path_; }
  void clear_in_memory() noexcept { events_.clear(); }

 private:
  std::string path_;
  std::size_t capacity_;
  std::vector<EpochEvent> events_;
  std::uint64_t replayed_records_ = 0;
  bool tail_damaged_ = false;
  std::string diagnostic_;
};

[[nodiscard]] Json to_json(const EpochEvent& event);
[[nodiscard]] Result<EpochEvent> epoch_event_from_json(const Json& value);
[[nodiscard]] std::string encode_epoch_event(const EpochEvent& event);

}  // namespace fabric::evolution
