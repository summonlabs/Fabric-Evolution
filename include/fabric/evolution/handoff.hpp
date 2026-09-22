// Fabric Evolution — handoff lifecycle.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// A handoff is the ordered sequence that moves mutating authority for one shard
// from a predecessor incarnation to a successor incarnation without stopping the
// control plane:
//
//   Prepared -> SnapshotSynchronized -> CaughtUp -> ReadinessVerified
//            -> PredecessorFenced -> AuthorityTransferred -> SuccessorVerified
//            -> PredecessorRetired
//
// The order is total and every transition is durable before it is acknowledged,
// so a controller that dies between two phases resumes from the persisted phase
// instead of guessing. Every phase transition is idempotent: replaying a phase
// that already completed is a no-op, which is what makes duplicate handoff
// frames harmless.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "fabric/evolution/authority.hpp"
#include "fabric/evolution/ids.hpp"
#include "fabric/evolution/manifest.hpp"

namespace fabric::evolution {

enum class HandoffPhase : std::uint8_t {
  NotStarted = 0,
  Prepared = 1,
  SnapshotSynchronized = 2,
  CaughtUp = 3,
  ReadinessVerified = 4,
  PredecessorFenced = 5,
  AuthorityTransferred = 6,
  SuccessorVerified = 7,
  PredecessorRetired = 8,
  Failed = 9,
  Aborted = 10,
};

[[nodiscard]] std::string_view to_string(HandoffPhase phase) noexcept;
[[nodiscard]] std::optional<HandoffPhase> handoff_phase_from(std::string_view text);

// True when the phase is one of the ordered forward phases.
[[nodiscard]] bool is_forward_phase(HandoffPhase phase) noexcept;
[[nodiscard]] std::optional<HandoffPhase> next_phase(HandoffPhase phase);
[[nodiscard]] bool is_terminal_phase(HandoffPhase phase) noexcept;
// The phase index used for "have we reached at least this point" comparisons.
[[nodiscard]] int phase_rank(HandoffPhase phase) noexcept;

// Authority must not be split while the fence is outstanding. This is the window
// in which pause and abort are deferred rather than honoured.
[[nodiscard]] bool is_authority_transition_window(HandoffPhase phase) noexcept;

struct PhaseTransition {
  HandoffPhase from = HandoffPhase::NotStarted;
  HandoffPhase to = HandoffPhase::NotStarted;
  TimestampMs at_ms = 0;
  AttemptNumber attempt;
  std::string note;
};

struct HandoffRecord {
  HandoffId id;
  CampaignId campaign;
  ShardId shard;
  ManifestId manifest;
  Digest manifest_digest;
  IncarnationId predecessor;
  // When the predecessor process is replaced before authority moves, the handoff
  // is restarted for the new incarnation and the replaced one is recorded here.
  IncarnationId previous_predecessor;
  IncarnationId successor;
  HandoffPhase phase = HandoffPhase::NotStarted;
  AttemptNumber attempt = AttemptNumber::from_value(1);
  EpochNumber epoch;
  Generation generation;
  SnapshotId snapshot;
  Digest snapshot_digest;
  LogSequenceNumber snapshot_lsn;
  LogSequenceNumber target_lsn;
  AuthorityToken token;
  FenceId fence;
  TimestampMs started_at_ms = 0;
  TimestampMs updated_at_ms = 0;
  std::string last_error;
  std::vector<PhaseTransition> history;

  [[nodiscard]] bool reached(HandoffPhase other) const noexcept {
    return phase_rank(phase) >= phase_rank(other) && is_forward_phase(phase);
  }
};

inline constexpr std::size_t kMaxHandoffHistoryEntries = 256;

// Deterministic transition validation. Any transition that is not strictly
// forward, not an entry into a terminal phase, and not a no-op is refused.
[[nodiscard]] Status validate_handoff_transition(HandoffPhase from, HandoffPhase to);

[[nodiscard]] Json to_json(const HandoffRecord& record);
[[nodiscard]] Result<HandoffRecord> handoff_record_from_json(const Json& value);

}  // namespace fabric::evolution
