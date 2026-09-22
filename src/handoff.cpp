// Fabric Evolution — handoff lifecycle (implementation).
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric/evolution/handoff.hpp"

#include <algorithm>

namespace fabric::evolution {
namespace {

constexpr HandoffPhase kForwardPhases[] = {
    HandoffPhase::NotStarted,         HandoffPhase::Prepared,
    HandoffPhase::SnapshotSynchronized, HandoffPhase::CaughtUp,
    HandoffPhase::ReadinessVerified,  HandoffPhase::PredecessorFenced,
    HandoffPhase::AuthorityTransferred, HandoffPhase::SuccessorVerified,
    HandoffPhase::PredecessorRetired,
};

}  // namespace

std::string_view to_string(HandoffPhase phase) noexcept {
  switch (phase) {
    case HandoffPhase::NotStarted:
      return "not_started";
    case HandoffPhase::Prepared:
      return "prepared";
    case HandoffPhase::SnapshotSynchronized:
      return "snapshot_synchronized";
    case HandoffPhase::CaughtUp:
      return "caught_up";
    case HandoffPhase::ReadinessVerified:
      return "readiness_verified";
    case HandoffPhase::PredecessorFenced:
      return "predecessor_fenced";
    case HandoffPhase::AuthorityTransferred:
      return "authority_transferred";
    case HandoffPhase::SuccessorVerified:
      return "successor_verified";
    case HandoffPhase::PredecessorRetired:
      return "predecessor_retired";
    case HandoffPhase::Failed:
      return "failed";
    case HandoffPhase::Aborted:
      return "aborted";
  }
  return "unknown";
}

std::optional<HandoffPhase> handoff_phase_from(std::string_view text) {
  for (HandoffPhase phase : kForwardPhases) {
    if (to_string(phase) == text) {
      return phase;
    }
  }
  if (text == "failed") {
    return HandoffPhase::Failed;
  }
  if (text == "aborted") {
    return HandoffPhase::Aborted;
  }
  return std::nullopt;
}

bool is_forward_phase(HandoffPhase phase) noexcept {
  return phase != HandoffPhase::Failed && phase != HandoffPhase::Aborted;
}

int phase_rank(HandoffPhase phase) noexcept {
  for (std::size_t index = 0; index < std::size(kForwardPhases); ++index) {
    if (kForwardPhases[index] == phase) {
      return static_cast<int>(index);
    }
  }
  return -1;
}

std::optional<HandoffPhase> next_phase(HandoffPhase phase) {
  if (!is_forward_phase(phase)) {
    return std::nullopt;
  }
  const int rank = phase_rank(phase);
  if (rank < 0 || static_cast<std::size_t>(rank) + 1 >= std::size(kForwardPhases)) {
    return std::nullopt;
  }
  return kForwardPhases[static_cast<std::size_t>(rank) + 1];
}

bool is_terminal_phase(HandoffPhase phase) noexcept {
  return phase == HandoffPhase::PredecessorRetired || phase == HandoffPhase::Failed ||
         phase == HandoffPhase::Aborted;
}

bool is_authority_transition_window(HandoffPhase phase) noexcept {
  return phase == HandoffPhase::PredecessorFenced || phase == HandoffPhase::AuthorityTransferred;
}

Status validate_handoff_transition(HandoffPhase from, HandoffPhase to) {
  if (from == to) {
    return Status::success();  // idempotent replay
  }
  if (!is_forward_phase(from) && from != HandoffPhase::NotStarted) {
    return Status::error(ErrorCode::IllegalTransition, "handoff is already in a terminal phase",
                         Json({{"from", Json(std::string(::fabric::evolution::to_string(from)))},
                               {"to", Json(std::string(::fabric::evolution::to_string(to)))}}));
  }
  if (to == HandoffPhase::Failed || to == HandoffPhase::Aborted) {
    return Status::success();
  }
  const auto expected = next_phase(from);
  if (!expected.has_value() || *expected != to) {
    return Status::error(
        ErrorCode::IllegalTransition, "handoff phases must advance one step at a time",
        Json({{"from", Json(std::string(::fabric::evolution::to_string(from)))},
              {"to", Json(std::string(::fabric::evolution::to_string(to)))},
              {"expected", Json(expected.has_value()
                                    ? std::string(::fabric::evolution::to_string(*expected))
                                    : std::string("none"))}}));
  }
  return Status::success();
}

Json to_json(const HandoffRecord& record) {
  Json out = Json::object();
  out.set("id", Json(record.id.value()));
  out.set("campaign", Json(record.campaign.str()));
  out.set("shard", Json(record.shard.str()));
  out.set("manifest", Json(record.manifest.str()));
  out.set("manifest_digest", Json(record.manifest_digest.to_hex()));
  out.set("predecessor", to_json(record.predecessor));
  if (record.previous_predecessor.is_valid()) {
    out.set("previous_predecessor", to_json(record.previous_predecessor));
  }
  out.set("successor", to_json(record.successor));
  out.set("phase", Json(std::string(::fabric::evolution::to_string(record.phase))));
  out.set("attempt", Json(record.attempt.value()));
  out.set("epoch", Json(record.epoch.value()));
  out.set("generation", Json(record.generation.value()));
  out.set("snapshot", Json(record.snapshot.value()));
  out.set("snapshot_digest", Json(record.snapshot_digest.to_hex()));
  out.set("snapshot_lsn", Json(record.snapshot_lsn.value()));
  out.set("target_lsn", Json(record.target_lsn.value()));
  out.set("token", Json(record.token.to_compact_string()));
  out.set("fence", Json(record.fence.value()));
  out.set("started_at_ms", Json(record.started_at_ms));
  out.set("updated_at_ms", Json(record.updated_at_ms));
  out.set("last_error", Json(record.last_error));
  Json history = Json::array();
  for (const PhaseTransition& transition : record.history) {
    Json entry = Json::object();
    entry.set("from", Json(std::string(::fabric::evolution::to_string(transition.from))));
    entry.set("to", Json(std::string(::fabric::evolution::to_string(transition.to))));
    entry.set("at_ms", Json(transition.at_ms));
    entry.set("attempt", Json(transition.attempt.value()));
    entry.set("note", Json(transition.note));
    history.push_back(std::move(entry));
  }
  out.set("history", std::move(history));
  return out;
}

Result<HandoffRecord> handoff_record_from_json(const Json& value) {
  if (!value.is_object()) {
    return malformed("handoff record must be an object");
  }
  HandoffRecord record;
  const auto id = json_u64(value, "id");
  if (!id.has_value() || *id == 0) {
    return malformed("handoff record id must be positive");
  }
  record.id = HandoffId::from_value(*id);

  const auto campaign = json_string(value, "campaign");
  const auto shard = json_string(value, "shard");
  const auto manifest = json_string(value, "manifest");
  if (!campaign.has_value() || !shard.has_value() || !manifest.has_value()) {
    return malformed("handoff record is missing campaign, shard or manifest");
  }
  auto parsed_campaign = CampaignId::parse(*campaign);
  auto parsed_shard = ShardId::parse(*shard);
  auto parsed_manifest = ManifestId::parse(*manifest);
  if (!parsed_campaign.has_value() || !parsed_shard.has_value() || !parsed_manifest.has_value()) {
    return malformed("handoff record identity is invalid");
  }
  record.campaign = *parsed_campaign;
  record.shard = *parsed_shard;
  record.manifest = *parsed_manifest;

  const auto manifest_digest = json_string(value, "manifest_digest");
  if (manifest_digest.has_value()) {
    auto parsed = Digest::from_hex(*manifest_digest);
    if (!parsed.has_value()) {
      return malformed("handoff record manifest_digest is not a sha256 hex digest");
    }
    record.manifest_digest = *parsed;
  }

  const Json* predecessor = value.find("predecessor");
  const Json* successor = value.find("successor");
  if (predecessor == nullptr || successor == nullptr) {
    return malformed("handoff record must name predecessor and successor incarnations");
  }
  auto parsed_predecessor = incarnation_from_json(*predecessor);
  if (!parsed_predecessor.ok()) {
    return parsed_predecessor.status();
  }
  record.predecessor = parsed_predecessor.value();
  if (const Json* previous = value.find("previous_predecessor")) {
    auto parsed_previous = incarnation_from_json(*previous);
    if (parsed_previous.ok()) {
      record.previous_predecessor = parsed_previous.value();
    }
  }
  auto parsed_successor = incarnation_from_json(*successor);
  if (!parsed_successor.ok()) {
    return parsed_successor.status();
  }
  record.successor = parsed_successor.value();

  const auto phase = json_string(value, "phase");
  if (!phase.has_value()) {
    return malformed("handoff record is missing phase");
  }
  auto parsed_phase = handoff_phase_from(*phase);
  if (!parsed_phase.has_value()) {
    return malformed("handoff record phase is invalid: " + *phase);
  }
  record.phase = *parsed_phase;

  record.attempt = AttemptNumber::from_value(static_cast<std::uint32_t>(json_u64_or(value, "attempt", 1)));
  record.epoch = EpochNumber::from_value(json_u64_or(value, "epoch", 0));
  record.generation = Generation::from_value(json_u64_or(value, "generation", 0));
  record.snapshot = SnapshotId::from_value(json_u64_or(value, "snapshot", 0));
  record.snapshot_lsn = LogSequenceNumber::from_value(json_u64_or(value, "snapshot_lsn", 0));
  record.target_lsn = LogSequenceNumber::from_value(json_u64_or(value, "target_lsn", 0));
  record.fence = FenceId::from_value(json_u64_or(value, "fence", 0));
  record.started_at_ms = json_u64_or(value, "started_at_ms", 0);
  record.updated_at_ms = json_u64_or(value, "updated_at_ms", 0);
  record.last_error = json_string_or(value, "last_error", "");

  if (const auto digest = json_string(value, "snapshot_digest")) {
    auto parsed = Digest::from_hex(*digest);
    if (!parsed.has_value()) {
      return malformed("handoff record snapshot_digest is not a sha256 hex digest");
    }
    record.snapshot_digest = *parsed;
  }
  if (const auto token = json_string(value, "token")) {
    auto parsed = AuthorityToken::parse(*token);
    if (!parsed.has_value()) {
      return malformed("handoff record token is not a valid uuid");
    }
    record.token = *parsed;
  }

  if (const Json* history = value.find("history")) {
    if (!history->is_array()) {
      return malformed("handoff record history must be an array");
    }
    for (std::size_t index = 0; index < history->size(); ++index) {
      const Json& entry = history->at(index);
      const auto from = json_string(entry, "from");
      const auto to = json_string(entry, "to");
      if (!from.has_value() || !to.has_value()) {
        return malformed("handoff history entry is missing from/to");
      }
      auto parsed_from = handoff_phase_from(*from);
      auto parsed_to = handoff_phase_from(*to);
      if (!parsed_from.has_value() || !parsed_to.has_value()) {
        return malformed("handoff history entry phase is invalid");
      }
      PhaseTransition transition;
      transition.from = *parsed_from;
      transition.to = *parsed_to;
      transition.at_ms = json_u64_or(entry, "at_ms", 0);
      transition.attempt = AttemptNumber::from_value(
          static_cast<std::uint32_t>(json_u64_or(entry, "attempt", 1)));
      transition.note = json_string_or(entry, "note", "");
      record.history.push_back(std::move(transition));
    }
  }
  return record;
}

}  // namespace fabric::evolution
