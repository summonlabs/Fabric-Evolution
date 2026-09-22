// Fabric Evolution — evolution campaign state machine (implementation).
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric/evolution/campaign.hpp"

namespace fabric::evolution {

std::string_view to_string(CampaignState state) noexcept {
  switch (state) {
    case CampaignState::Planned:
      return "planned";
    case CampaignState::Preflighting:
      return "preflighting";
    case CampaignState::Running:
      return "running";
    case CampaignState::Paused:
      return "paused";
    case CampaignState::Aborting:
      return "aborting";
    case CampaignState::Aborted:
      return "aborted";
    case CampaignState::ForwardRecovery:
      return "forward_recovery";
    case CampaignState::Completed:
      return "completed";
    case CampaignState::Failed:
      return "failed";
  }
  return "unknown";
}

std::optional<CampaignState> campaign_state_from(std::string_view text) {
  const CampaignState states[] = {
      CampaignState::Planned,     CampaignState::Preflighting,    CampaignState::Running,
      CampaignState::Paused,      CampaignState::Aborting,        CampaignState::Aborted,
      CampaignState::ForwardRecovery, CampaignState::Completed,  CampaignState::Failed,
  };
  for (CampaignState state : states) {
    if (to_string(state) == text) {
      return state;
    }
  }
  return std::nullopt;
}

bool is_terminal_state(CampaignState state) noexcept {
  return state == CampaignState::Aborted || state == CampaignState::Completed ||
         state == CampaignState::Failed;
}

std::string_view to_string(CampaignAction action) noexcept {
  switch (action) {
    case CampaignAction::Plan:
      return "plan";
    case CampaignAction::Preflight:
      return "preflight";
    case CampaignAction::Start:
      return "start";
    case CampaignAction::Pause:
      return "pause";
    case CampaignAction::Resume:
      return "resume";
    case CampaignAction::Abort:
      return "abort";
    case CampaignAction::Complete:
      return "complete";
    case CampaignAction::Fail:
      return "fail";
    case CampaignAction::EnterForwardRecovery:
      return "forward_recovery";
    case CampaignAction::ConfirmAbort:
      return "confirm_abort";
  }
  return "unknown";
}

ActionDecision evaluate_campaign_action(const CampaignRecord& record, CampaignAction action,
                                        const HandoffRecord& handoff, bool irreversible_crossed,
                                        const ManifestId& manifest_id) {
  ActionDecision decision;
  decision.next_state = record.state;

  const auto reject = [&decision](std::string rule, std::string reason) {
    decision.accepted = false;
    decision.rule = std::move(rule);
    decision.reason = std::move(reason);
    return decision;
  };

  if (record.manifest != manifest_id) {
    return reject("campaign.manifest_binding",
                  "the campaign is bound to a different manifest than the one supplied");
  }
  if (is_terminal_state(record.state)) {
    return reject("campaign.terminal", "the campaign has already reached a terminal state");
  }

  switch (action) {
    case CampaignAction::Plan:
      return reject("campaign.plan_once", "the campaign has already been planned");
    case CampaignAction::Preflight:
      if (record.state != CampaignState::Planned) {
        return reject("campaign.preflight_from_planned", "preflight only applies to a planned campaign");
      }
      decision.accepted = true;
      decision.next_state = CampaignState::Preflighting;
      decision.rule = "campaign.preflight_from_planned";
      decision.reason = "preflight is permitted for a planned campaign";
      return decision;
    case CampaignAction::Start:
      if (record.state != CampaignState::Preflighting && record.state != CampaignState::Paused) {
        return reject("campaign.start_requires_preflight",
                      "a campaign may only start after preflight, or resume from paused");
      }
      decision.accepted = true;
      decision.next_state = CampaignState::Running;
      decision.rule = "campaign.start_requires_preflight";
      decision.reason = "the campaign enters the running state";
      return decision;
    case CampaignAction::Pause:
      if (record.state != CampaignState::Running) {
        return reject("campaign.pause_from_running", "only a running campaign can be paused");
      }
      if (is_authority_transition_window(handoff.phase)) {
        decision.accepted = false;
        decision.deferred = true;
        decision.next_state = CampaignState::Running;
        decision.rule = "campaign.pause_deferred_in_authority_window";
        decision.reason =
            "authority is mid-transfer; pausing here could leave a shard without an owner, so the "
            "request is deferred to the next phase boundary";
        return decision;
      }
      decision.accepted = true;
      decision.next_state = CampaignState::Paused;
      decision.rule = "campaign.pause_from_running";
      decision.reason = "the campaign pauses at a safe phase boundary";
      return decision;
    case CampaignAction::Resume:
      if (record.state != CampaignState::Paused) {
        return reject("campaign.resume_from_paused", "only a paused campaign can be resumed");
      }
      decision.accepted = true;
      decision.next_state = CampaignState::Running;
      decision.rule = "campaign.resume_from_paused";
      decision.reason = "the campaign resumes";
      return decision;
    case CampaignAction::Abort:
      if (record.state == CampaignState::Aborting) {
        decision.accepted = true;
        decision.next_state = CampaignState::Aborting;
        decision.rule = "campaign.abort_idempotent";
        decision.reason = "the campaign is already aborting";
        return decision;
      }
      if (irreversible_crossed || record.irreversible_boundary_crossed ||
          handoff.reached(HandoffPhase::AuthorityTransferred)) {
        decision.accepted = true;
        decision.next_state = CampaignState::ForwardRecovery;
        decision.rule = "campaign.abort_forward_recovery";
        decision.reason =
            "an irreversible migration boundary has been crossed or authority has already moved, so "
            "rolling back is unsafe and the campaign continues forward instead";
        return decision;
      }
      decision.accepted = true;
      decision.next_state = CampaignState::Aborting;
      decision.rule = "campaign.abort_reversible";
      decision.reason = "no irreversible boundary has been crossed, so the campaign can roll back";
      return decision;
    case CampaignAction::Complete:
      if (record.state != CampaignState::Running && record.state != CampaignState::ForwardRecovery) {
        return reject("campaign.complete_from_running",
                      "a campaign can only complete from running or forward recovery");
      }
      if (!handoff.reached(HandoffPhase::SuccessorVerified)) {
        return reject("campaign.complete_requires_verified_successor",
                      "the successor has not been verified as authoritative");
      }
      decision.accepted = true;
      decision.next_state = CampaignState::Completed;
      decision.rule = "campaign.complete_requires_verified_successor";
      decision.reason = "the successor is verified and the predecessor is retired";
      return decision;
    case CampaignAction::Fail:
      decision.accepted = true;
      decision.next_state = CampaignState::Failed;
      decision.rule = "campaign.fail_on_error";
      decision.reason = "the campaign cannot make progress safely";
      return decision;
    case CampaignAction::EnterForwardRecovery:
      decision.accepted = true;
      decision.next_state = CampaignState::ForwardRecovery;
      decision.rule = "campaign.forward_recovery";
      decision.reason = "the campaign continues forward because rollback is unsafe";
      return decision;
    case CampaignAction::ConfirmAbort:
      if (record.state != CampaignState::Aborting) {
        return reject("campaign.confirm_abort_from_aborting",
                      "only a campaign that is aborting can be confirmed as aborted");
      }
      decision.accepted = true;
      decision.next_state = CampaignState::Aborted;
      decision.rule = "campaign.confirm_abort_from_aborting";
      decision.reason = "rollback completed and the campaign is aborted";
      return decision;
  }
  return reject("campaign.unknown_action", "unrecognised campaign action");
}

Json to_json(const CampaignRecord& record) {
  Json out = Json::object();
  out.set("id", Json(record.id.str()));
  out.set("manifest", Json(record.manifest.str()));
  out.set("manifest_digest", Json(record.manifest_digest.to_hex()));
  out.set("shard", Json(record.shard.str()));
  out.set("state", Json(std::string(::fabric::evolution::to_string(record.state))));
  out.set("handoff", Json(record.handoff.value()));
  out.set("revision", Json(record.revision.value()));
  out.set("epoch", Json(record.epoch.value()));
  out.set("generation", Json(record.generation.value()));
  out.set("operations_in_window", Json(record.operations_in_window));
  out.set("window_started_at_ms", Json(record.window_started_at_ms));
  out.set("updated_at_ms", Json(record.updated_at_ms));
  out.set("irreversible_boundary_crossed", Json(record.irreversible_boundary_crossed));
  Json boundaries = Json::array();
  for (const MigrationStepId& boundary : record.crossed_boundaries) {
    boundaries.push_back(Json(boundary.str()));
  }
  out.set("crossed_boundaries", std::move(boundaries));
  out.set("detail", Json(record.detail));
  Json events = Json::array();
  for (const CampaignEvent& event : record.events) {
    Json entry = Json::object();
    entry.set("from", Json(std::string(::fabric::evolution::to_string(event.from))));
    entry.set("to", Json(std::string(::fabric::evolution::to_string(event.to))));
    entry.set("action", Json(std::string(::fabric::evolution::to_string(event.action))));
    entry.set("at_ms", Json(event.at_ms));
    entry.set("detail", Json(event.detail));
    events.push_back(std::move(entry));
  }
  out.set("events", std::move(events));
  return out;
}

Result<CampaignRecord> campaign_record_from_json(const Json& value) {
  if (!value.is_object()) {
    return malformed("campaign record must be an object");
  }
  CampaignRecord record;
  const auto id = json_string(value, "id");
  const auto manifest = json_string(value, "manifest");
  const auto shard = json_string(value, "shard");
  if (!id.has_value() || !manifest.has_value() || !shard.has_value()) {
    return malformed("campaign record is missing id, manifest or shard");
  }
  auto parsed_id = CampaignId::parse(*id);
  auto parsed_manifest = ManifestId::parse(*manifest);
  auto parsed_shard = ShardId::parse(*shard);
  if (!parsed_id.has_value() || !parsed_manifest.has_value() || !parsed_shard.has_value()) {
    return malformed("campaign record identity is invalid");
  }
  record.id = *parsed_id;
  record.manifest = *parsed_manifest;
  record.shard = *parsed_shard;

  const auto digest = json_string(value, "manifest_digest");
  if (digest.has_value()) {
    auto parsed = Digest::from_hex(*digest);
    if (!parsed.has_value()) {
      return malformed("campaign record manifest_digest is not a sha256 hex digest");
    }
    record.manifest_digest = *parsed;
  }

  const auto state = json_string(value, "state");
  if (!state.has_value()) {
    return malformed("campaign record is missing state");
  }
  auto parsed_state = campaign_state_from(*state);
  if (!parsed_state.has_value()) {
    return malformed("campaign record state is invalid: " + *state);
  }
  record.state = *parsed_state;

  record.handoff = HandoffId::from_value(json_u64_or(value, "handoff", 0));
  record.revision = Revision::from_value(json_u64_or(value, "revision", 1));
  record.epoch = EpochNumber::from_value(json_u64_or(value, "epoch", 0));
  record.generation = Generation::from_value(json_u64_or(value, "generation", 0));
  record.operations_in_window = json_u64_or(value, "operations_in_window", 0);
  record.window_started_at_ms = json_u64_or(value, "window_started_at_ms", 0);
  record.updated_at_ms = json_u64_or(value, "updated_at_ms", 0);
  record.irreversible_boundary_crossed = json_bool_or(value, "irreversible_boundary_crossed", false);
  record.detail = json_string_or(value, "detail", "");

  if (const Json* boundaries = value.find("crossed_boundaries")) {
    if (!boundaries->is_array()) {
      return malformed("campaign record crossed_boundaries must be an array");
    }
    for (std::size_t index = 0; index < boundaries->size(); ++index) {
      const std::string* text = boundaries->at(index).try_string();
      if (text == nullptr) {
        return malformed("campaign record crossed boundary ids must be strings");
      }
      auto parsed = MigrationStepId::parse(*text);
      if (!parsed.has_value()) {
        return malformed("campaign record crossed boundary id is invalid: " + *text);
      }
      record.crossed_boundaries.push_back(*parsed);
    }
  }

  if (const Json* events = value.find("events")) {
    if (!events->is_array()) {
      return malformed("campaign record events must be an array");
    }
    for (std::size_t index = 0; index < events->size(); ++index) {
      const Json& entry = events->at(index);
      const auto from = json_string(entry, "from");
      const auto to = json_string(entry, "to");
      const auto action = json_string(entry, "action");
      if (!from.has_value() || !to.has_value() || !action.has_value()) {
        return malformed("campaign event is missing from/to/action");
      }
      auto parsed_from = campaign_state_from(*from);
      auto parsed_to = campaign_state_from(*to);
      if (!parsed_from.has_value() || !parsed_to.has_value()) {
        return malformed("campaign event state is invalid");
      }
      CampaignEvent event;
      event.from = *parsed_from;
      event.to = *parsed_to;
      event.at_ms = json_u64_or(entry, "at_ms", 0);
      event.detail = json_string_or(entry, "detail", "");
      if (*action == "plan") {
        event.action = CampaignAction::Plan;
      } else if (*action == "preflight") {
        event.action = CampaignAction::Preflight;
      } else if (*action == "start") {
        event.action = CampaignAction::Start;
      } else if (*action == "pause") {
        event.action = CampaignAction::Pause;
      } else if (*action == "resume") {
        event.action = CampaignAction::Resume;
      } else if (*action == "abort") {
        event.action = CampaignAction::Abort;
      } else if (*action == "complete") {
        event.action = CampaignAction::Complete;
      } else if (*action == "fail") {
        event.action = CampaignAction::Fail;
      } else if (*action == "forward_recovery") {
        event.action = CampaignAction::EnterForwardRecovery;
      } else if (*action == "confirm_abort") {
        event.action = CampaignAction::ConfirmAbort;
      } else {
        return malformed("campaign event action is invalid: " + *action);
      }
      record.events.push_back(std::move(event));
    }
  }
  return record;
}

WindowStatus evaluate_window(const CampaignRecord& record, const MixedVersionWindow& window,
                             TimestampMs now_ms) {
  WindowStatus status;
  status.operation_limit = window.max_operations;
  status.duration_limit_ms = window.max_duration_ms;
  if (record.window_started_at_ms == 0) {
    status.open = false;
    status.reason = "the mixed-version window has not opened";
    return status;
  }
  status.open = true;
  status.opened_at_ms = record.window_started_at_ms;
  status.operations = record.operations_in_window;
  status.elapsed_ms = now_ms > record.window_started_at_ms ? now_ms - record.window_started_at_ms : 0;
  if (window.max_operations != 0 && status.operations > window.max_operations) {
    status.exhausted = true;
    status.reason = "the mixed-version operation budget is exhausted";
    return status;
  }
  if (window.max_duration_ms != 0 && status.elapsed_ms > window.max_duration_ms) {
    status.exhausted = true;
    status.reason = "the mixed-version time budget is exhausted";
    return status;
  }
  status.reason = "the mixed-version window is open";
  return status;
}

}  // namespace fabric::evolution
