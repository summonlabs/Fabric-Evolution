// Fabric Evolution — deterministic decision explanations (implementation).
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric/evolution/explain.hpp"

namespace fabric::evolution {

std::string_view to_string(DecisionOutcome outcome) noexcept {
  switch (outcome) {
    case DecisionOutcome::Accepted:
      return "accepted";
    case DecisionOutcome::Rejected:
      return "rejected";
    case DecisionOutcome::Deferred:
      return "deferred";
  }
  return "unknown";
}

Explanation::Explanation(std::string decision, std::string policy)
    : decision_(std::move(decision)), policy_(std::move(policy)), inputs_(Json::object()),
      evidence_(Json::object()) {}

void Explanation::add_rejected_alternative(std::string action, std::string reason) {
  rejected_.push_back(RejectedAlternative{std::move(action), std::move(reason)});
}

void Explanation::add_note(std::string note) { notes_.push_back(std::move(note)); }

Json Explanation::to_json() const {
  Json out = Json::object();
  out.set("decision", Json(decision_));
  out.set("outcome", Json(std::string(::fabric::evolution::to_string(outcome_))));
  out.set("policy", Json(policy_));
  out.set("inputs", inputs_);
  out.set("evidence", evidence_);
  if (!selected_action_.empty()) {
    out.set("selected_action", Json(selected_action_));
  }
  if (!resulting_state_.empty()) {
    out.set("resulting_state", Json(resulting_state_));
  }
  Json authority = Json::object();
  authority.set("epoch", Json(authority_.epoch.value()));
  authority.set("generation", Json(authority_.generation.value()));
  if (authority_.actor.has_value()) {
    authority.set("actor", ::fabric::evolution::to_json(*authority_.actor));
  }
  if (authority_.token.has_value()) {
    authority.set("token", Json(authority_.token->to_compact_string()));
  }
  out.set("authority", std::move(authority));
  Json rejected = Json::array();
  for (const RejectedAlternative& alternative : rejected_) {
    rejected.push_back(Json::object({{"action", Json(alternative.action)}, {"reason", Json(alternative.reason)}}));
  }
  out.set("rejected_alternatives", std::move(rejected));
  if (!notes_.empty()) {
    Json notes = Json::array();
    for (const std::string& note : notes_) {
      notes.push_back(Json(note));
    }
    out.set("notes", std::move(notes));
  }
  out.set("decided_at_ms", Json(decided_at_ms_));
  return out;
}

std::string Explanation::summary() const {
  std::string out;
  out += decision_;
  out += " [";
  out += ::fabric::evolution::to_string(outcome_);
  out += "] policy=";
  out += policy_;
  out += " epoch=";
  out += std::to_string(authority_.epoch.value());
  out += " generation=";
  out += std::to_string(authority_.generation.value());
  if (!selected_action_.empty()) {
    out += " action=";
    out += selected_action_;
  }
  if (!resulting_state_.empty()) {
    out += " state=";
    out += resulting_state_;
  }
  for (const RejectedAlternative& alternative : rejected_) {
    out += " | rejected ";
    out += alternative.action;
    out += " (";
    out += alternative.reason;
    out += ")";
  }
  return out;
}

}  // namespace fabric::evolution
