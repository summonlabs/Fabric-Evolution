// Fabric Evolution — deterministic decision explanations.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every policy decision the runtime makes — accepting a campaign, crossing an
// irreversible boundary, refusing a handoff, fencing a predecessor — produces an
// Explanation. An explanation states the inputs it saw, the evidence it used,
// the policy that governed it, the action it selected, the alternatives it
// rejected, and the epoch/generation/incarnation under which it decided. Two
// runs with the same inputs and evidence produce byte-identical explanations.

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "fabric/evolution/clock.hpp"
#include "fabric/evolution/ids.hpp"
#include "fabric/evolution/json.hpp"

namespace fabric::evolution {

enum class DecisionOutcome : std::uint8_t {
  Accepted = 0,
  Rejected = 1,
  Deferred = 2,
};

[[nodiscard]] std::string_view to_string(DecisionOutcome outcome) noexcept;

struct RejectedAlternative {
  std::string action;
  std::string reason;
};

// The authority context under which a decision was taken.
struct DecisionAuthority {
  EpochNumber epoch;
  Generation generation;
  std::optional<IncarnationId> actor;
  std::optional<AuthorityToken> token;

  [[nodiscard]] bool is_valid() const noexcept { return epoch.is_valid() && generation.is_valid(); }
};

class Explanation {
 public:
  Explanation(std::string decision, std::string policy);

  void set_outcome(DecisionOutcome outcome) { outcome_ = outcome; }
  void set_inputs(Json inputs) { inputs_ = std::move(inputs); }
  void set_evidence(Json evidence) { evidence_ = std::move(evidence); }
  void set_selected_action(std::string action) { selected_action_ = std::move(action); }
  void set_resulting_state(std::string state) { resulting_state_ = std::move(state); }
  void set_authority(DecisionAuthority authority) { authority_ = std::move(authority); }
  void set_decided_at(TimestampMs at_ms) { decided_at_ms_ = at_ms; }
  void add_rejected_alternative(std::string action, std::string reason);
  void add_note(std::string note);

  [[nodiscard]] DecisionOutcome outcome() const noexcept { return outcome_; }
  [[nodiscard]] const std::string& decision() const noexcept { return decision_; }
  [[nodiscard]] const std::string& policy() const noexcept { return policy_; }
  [[nodiscard]] const Json& inputs() const noexcept { return inputs_; }
  [[nodiscard]] const Json& evidence() const noexcept { return evidence_; }
  [[nodiscard]] const std::string& selected_action() const noexcept { return selected_action_; }
  [[nodiscard]] const std::string& resulting_state() const noexcept { return resulting_state_; }
  [[nodiscard]] const DecisionAuthority& authority() const noexcept { return authority_; }
  [[nodiscard]] const std::vector<RejectedAlternative>& rejected_alternatives() const noexcept {
    return rejected_;
  }
  [[nodiscard]] const std::vector<std::string>& notes() const noexcept { return notes_; }

  [[nodiscard]] Json to_json() const;
  // Stable single-line rendering used by the CLI and by tests.
  [[nodiscard]] std::string summary() const;

 private:
  std::string decision_;
  std::string policy_;
  DecisionOutcome outcome_ = DecisionOutcome::Deferred;
  Json inputs_;
  Json evidence_;
  std::string selected_action_;
  std::string resulting_state_;
  DecisionAuthority authority_;
  std::vector<RejectedAlternative> rejected_;
  std::vector<std::string> notes_;
  TimestampMs decided_at_ms_ = 0;
};

}  // namespace fabric::evolution
