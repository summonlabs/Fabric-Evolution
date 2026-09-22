// Fabric Evolution — evolution campaign state machine.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// A campaign is the durable, restart-safe record of one controlled evolution of
// one shard. It owns the mixed-version window accounting, the pause/abort
// semantics and the decision of whether a stop rolls back or goes forward.
//
// Pause is honoured only at a phase boundary outside the authority-transfer
// window; a pause request that arrives inside that window is deferred with a
// deterministic reason rather than silently dropped. Abort rolls back while no
// irreversible boundary has been crossed and switches the campaign to forward
// recovery once one has been, which is the only safe behaviour.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "fabric/evolution/clock.hpp"
#include "fabric/evolution/handoff.hpp"
#include "fabric/evolution/manifest.hpp"

namespace fabric::evolution {

enum class CampaignState : std::uint8_t {
  Planned = 0,
  Preflighting = 1,
  Running = 2,
  Paused = 3,
  Aborting = 4,
  Aborted = 5,
  ForwardRecovery = 6,
  Completed = 7,
  Failed = 8,
};

[[nodiscard]] std::string_view to_string(CampaignState state) noexcept;
[[nodiscard]] std::optional<CampaignState> campaign_state_from(std::string_view text);
[[nodiscard]] bool is_terminal_state(CampaignState state) noexcept;

enum class CampaignAction : std::uint8_t {
  Plan = 0,
  Preflight = 1,
  Start = 2,
  Pause = 3,
  Resume = 4,
  Abort = 5,
  Complete = 6,
  Fail = 7,
  EnterForwardRecovery = 8,
  ConfirmAbort = 9,
};

[[nodiscard]] std::string_view to_string(CampaignAction action) noexcept;

struct CampaignEvent {
  CampaignState from = CampaignState::Planned;
  CampaignState to = CampaignState::Planned;
  CampaignAction action = CampaignAction::Plan;
  TimestampMs at_ms = 0;
  std::string detail;
};

struct CampaignRecord {
  CampaignId id;
  ManifestId manifest;
  Digest manifest_digest;
  ShardId shard;
  CampaignState state = CampaignState::Planned;
  HandoffId handoff;
  Revision revision = Revision::from_value(1);
  EpochNumber epoch;
  Generation generation;
  std::uint64_t operations_in_window = 0;
  TimestampMs window_started_at_ms = 0;
  TimestampMs updated_at_ms = 0;
  bool irreversible_boundary_crossed = false;
  std::vector<MigrationStepId> crossed_boundaries;
  std::string detail;
  std::vector<CampaignEvent> events;
};

inline constexpr std::size_t kMaxCampaignEvents = 256;

// Result of evaluating whether an action may be applied. When accepted is false,
// the explanation states the governing rule and the rejected alternative.
struct ActionDecision {
  bool accepted = false;
  CampaignState next_state = CampaignState::Planned;
  bool deferred = false;
  std::string rule;
  std::string reason;
};

// Pure decision function: the campaign state machine has no hidden state, so the
// same record and the same action always produce the same decision.
[[nodiscard]] ActionDecision evaluate_campaign_action(const CampaignRecord& record,
                                                      CampaignAction action,
                                                      const HandoffRecord& handoff,
                                                      bool irreversible_crossed,
                                                      const ManifestId& manifest_id);

[[nodiscard]] Json to_json(const CampaignRecord& record);
[[nodiscard]] Result<CampaignRecord> campaign_record_from_json(const Json& value);

// Mixed-version window accounting. The window opens when the successor first
// participates and must be closed (or the campaign aborted) before either bound
// is exhausted.
struct WindowStatus {
  bool open = false;
  TimestampMs opened_at_ms = 0;
  std::uint64_t operations = 0;
  std::uint64_t operation_limit = 0;
  std::uint64_t duration_limit_ms = 0;
  std::uint64_t elapsed_ms = 0;
  bool exhausted = false;
  std::string reason;
};

[[nodiscard]] WindowStatus evaluate_window(const CampaignRecord& record,
                                           const MixedVersionWindow& window, TimestampMs now_ms);

}  // namespace fabric::evolution
