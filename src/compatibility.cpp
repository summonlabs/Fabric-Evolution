// Fabric Evolution — consumed compatibility decisions (implementation).
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric/evolution/compatibility.hpp"

#include <fstream>
#include <sstream>

#include "fabric/evolution/json.hpp"

namespace fabric::evolution {
namespace {

[[nodiscard]] Json constraint_array(const std::vector<CompatibilityConstraint>& constraints) {
  Json out = Json::array();
  for (const CompatibilityConstraint& constraint : constraints) {
    out.push_back(Json::object({{"name", Json(constraint.name)}, {"value", Json(constraint.value)}}));
  }
  return out;
}

[[nodiscard]] Json feature_array(const FeatureSet& features) {
  Json out = Json::array();
  for (const std::string& name : features.names()) {
    out.push_back(Json(name));
  }
  return out;
}

[[nodiscard]] Result<std::uint64_t> require_u64(const Json& value, const char* key) {
  const auto number = json_u64(value, key);
  if (!number.has_value()) {
    return malformed(std::string("compatibility decision field '") + key + "' must be a non-negative integer");
  }
  return *number;
}

}  // namespace

std::string_view to_string(CompatibilityVerdict verdict) noexcept {
  switch (verdict) {
    case CompatibilityVerdict::Unsupported:
      return "unsupported";
    case CompatibilityVerdict::SupportedWithConstraints:
      return "supported_with_constraints";
    case CompatibilityVerdict::Supported:
      return "supported";
  }
  return "unknown";
}

std::optional<CompatibilityVerdict> compatibility_verdict_from(std::string_view text) {
  if (text == "unsupported") {
    return CompatibilityVerdict::Unsupported;
  }
  if (text == "supported_with_constraints") {
    return CompatibilityVerdict::SupportedWithConstraints;
  }
  if (text == "supported") {
    return CompatibilityVerdict::Supported;
  }
  return std::nullopt;
}

Digest CompatibilityDecision::compute_digest() const {
  Json out = Json::object();
  out.set("decision_id", Json(decision_id));
  out.set("registry", Json(registry));
  out.set("subject", Json(subject.str()));
  out.set("source", Json(source.to_string()));
  out.set("target", Json(target.to_string()));
  out.set("source_protocol", Json(source_protocol.to_string()));
  out.set("target_protocol", Json(target_protocol.to_string()));
  out.set("source_schema", Json(source_schema.value()));
  out.set("target_schema", Json(target_schema.value()));
  out.set("verdict", Json(std::string(::fabric::evolution::to_string(verdict))));
  out.set("rollback_permitted", Json(rollback_permitted));
  out.set("max_mixed_version_operations", Json(max_mixed_version_operations));
  out.set("max_mixed_version_ms", Json(max_mixed_version_ms));
  out.set("certified_features", feature_array(certified_features));
  out.set("constraints", constraint_array(constraints));
  out.set("decided_at", Json(decided_at.value()));
  return sha256(out.dump());
}

Status CompatibilityDecision::seal() {
  evidence_digest = compute_digest();
  return Status::success();
}

Status CompatibilityDecision::verify_digest() const {
  const Digest recomputed = compute_digest();
  if (recomputed != evidence_digest) {
    return Status::error(ErrorCode::IntegrityFailure, "compatibility decision evidence digest mismatch",
                         Json::object({{"recorded", Json(evidence_digest.to_hex())},
                               {"recomputed", Json(recomputed.to_hex())}}));
  }
  return Status::success();
}

Json to_json(const CompatibilityDecision& decision) {
  Json out = Json::object();
  out.set("decision_id", Json(decision.decision_id));
  out.set("registry", Json(decision.registry));
  out.set("subject", Json(decision.subject.str()));
  out.set("source", Json(decision.source.to_string()));
  out.set("target", Json(decision.target.to_string()));
  out.set("source_protocol", Json(decision.source_protocol.to_string()));
  out.set("target_protocol", Json(decision.target_protocol.to_string()));
  out.set("source_schema", Json(decision.source_schema.value()));
  out.set("target_schema", Json(decision.target_schema.value()));
  out.set("verdict", Json(std::string(::fabric::evolution::to_string(decision.verdict))));
  out.set("rollback_permitted", Json(decision.rollback_permitted));
  out.set("max_mixed_version_operations", Json(decision.max_mixed_version_operations));
  out.set("max_mixed_version_ms", Json(decision.max_mixed_version_ms));
  out.set("certified_features", feature_array(decision.certified_features));
  out.set("constraints", constraint_array(decision.constraints));
  out.set("decided_at", Json(decision.decided_at.value()));
  out.set("evidence_digest", Json(decision.evidence_digest.to_hex()));
  return out;
}

Result<CompatibilityDecision> compatibility_decision_from_json(const Json& value) {
  if (!value.is_object()) {
    return malformed("compatibility decision must be an object");
  }
  CompatibilityDecision decision;

  const auto decision_id = json_string(value, "decision_id");
  if (!decision_id.has_value() || decision_id->empty()) {
    return malformed("compatibility decision is missing decision_id");
  }
  decision.decision_id = *decision_id;
  decision.registry = json_string_or(value, "registry", "fabric-compatibility-registry");

  const auto subject = json_string(value, "subject");
  if (!subject.has_value()) {
    return malformed("compatibility decision is missing subject");
  }
  auto subject_id = ComponentId::parse(*subject);
  if (!subject_id.has_value()) {
    return malformed("compatibility decision subject is invalid: " + *subject);
  }
  decision.subject = *subject_id;

  const auto source = json_string(value, "source");
  const auto target = json_string(value, "target");
  if (!source.has_value() || !target.has_value()) {
    return malformed("compatibility decision must name source and target software versions");
  }
  auto source_version = SoftwareVersion::parse(*source);
  auto target_version = SoftwareVersion::parse(*target);
  if (!source_version.has_value() || !target_version.has_value()) {
    return malformed("compatibility decision software versions are invalid");
  }
  decision.source = *source_version;
  decision.target = *target_version;

  const auto source_protocol = json_string(value, "source_protocol");
  const auto target_protocol = json_string(value, "target_protocol");
  if (!source_protocol.has_value() || !target_protocol.has_value()) {
    return malformed("compatibility decision must name source and target protocol versions");
  }
  auto protocol_source = ProtocolVersion::parse(*source_protocol);
  auto protocol_target = ProtocolVersion::parse(*target_protocol);
  if (!protocol_source.has_value() || !protocol_target.has_value()) {
    return malformed("compatibility decision protocol versions are invalid");
  }
  decision.source_protocol = *protocol_source;
  decision.target_protocol = *protocol_target;

  const auto source_schema = json_u64(value, "source_schema");
  const auto target_schema = json_u64(value, "target_schema");
  if (!source_schema.has_value() || !target_schema.has_value()) {
    return malformed("compatibility decision must name source and target schema versions");
  }
  if (*source_schema == 0 || *source_schema > UINT32_MAX || *target_schema == 0 ||
      *target_schema > UINT32_MAX) {
    return malformed("compatibility decision schema versions are out of range");
  }
  decision.source_schema = SchemaVersion::from_value(static_cast<std::uint32_t>(*source_schema));
  decision.target_schema = SchemaVersion::from_value(static_cast<std::uint32_t>(*target_schema));

  const auto verdict_text = json_string(value, "verdict");
  if (!verdict_text.has_value()) {
    return malformed("compatibility decision is missing verdict");
  }
  const auto verdict = compatibility_verdict_from(*verdict_text);
  if (!verdict.has_value()) {
    return malformed("compatibility decision verdict is invalid: " + *verdict_text);
  }
  decision.verdict = *verdict;

  decision.rollback_permitted = json_bool_or(value, "rollback_permitted", false);

  auto operations = require_u64(value, "max_mixed_version_operations");
  if (!operations.ok()) {
    return operations.status();
  }
  decision.max_mixed_version_operations = operations.value();
  auto duration = require_u64(value, "max_mixed_version_ms");
  if (!duration.ok()) {
    return duration.status();
  }
  decision.max_mixed_version_ms = duration.value();

  if (const Json* features = value.find("certified_features")) {
    if (!features->is_array()) {
      return malformed("compatibility decision certified_features must be an array");
    }
    std::vector<std::string> names;
    for (std::size_t index = 0; index < features->size(); ++index) {
      const std::string* name = features->at(index).try_string();
      if (name == nullptr) {
        return malformed("compatibility decision certified_features entries must be strings");
      }
      names.push_back(*name);
    }
    std::string error;
    const auto parsed = FeatureSet::from_names(names, error);
    if (!parsed.has_value()) {
      return malformed(error);
    }
    decision.certified_features = *parsed;
  }

  if (const Json* constraints = value.find("constraints")) {
    if (!constraints->is_array()) {
      return malformed("compatibility decision constraints must be an array");
    }
    for (std::size_t index = 0; index < constraints->size(); ++index) {
      const Json& entry = constraints->at(index);
      const auto name = json_string(entry, "name");
      const auto constraint_value = json_string(entry, "value");
      if (!name.has_value() || !constraint_value.has_value()) {
        return malformed("compatibility decision constraint entries need name and value");
      }
      decision.constraints.push_back(CompatibilityConstraint{*name, *constraint_value});
    }
  }

  decision.decided_at = EpochNumber::from_value(json_u64_or(value, "decided_at", 1));

  const auto digest_text = json_string(value, "evidence_digest");
  if (digest_text.has_value()) {
    auto digest = Digest::from_hex(*digest_text);
    if (!digest.has_value()) {
      return malformed("compatibility decision evidence_digest is not a sha256 hex digest");
    }
    decision.evidence_digest = *digest;
    const Status verified = decision.verify_digest();
    if (!verified.ok()) {
      return verified;
    }
  } else {
    const Status sealed = decision.seal();
    if (!sealed.ok()) {
      return sealed;
    }
  }
  return decision;
}

CompatibilityRegistry::~CompatibilityRegistry() = default;

void StaticCompatibilityRegistry::add(CompatibilityDecision decision) { decisions_.push_back(std::move(decision)); }

Result<CompatibilityDecision> StaticCompatibilityRegistry::decide(const ComponentId& subject,
                                                                  const SoftwareVersion& source,
                                                                  const SoftwareVersion& target) const {
  for (const CompatibilityDecision& decision : decisions_) {
    if (decision.subject == subject && decision.source == source && decision.target == target) {
      return decision;
    }
  }
  return Status::error(ErrorCode::CompatibilityInsufficient,
                       "no compatibility decision covers this component and version pair",
                       Json::object({{"subject", Json(subject.str())},
                             {"source", Json(source.to_string())},
                             {"target", Json(target.to_string())}}));
}

Result<JsonCompatibilityRegistry> JsonCompatibilityRegistry::parse(std::string_view text) {
  const JsonParseResult parsed = parse_json(text);
  if (!parsed.ok()) {
    return malformed("compatibility registry document is not valid JSON: " + parsed.error);
  }
  const Json& document = *parsed.value;
  if (!document.is_object()) {
    return malformed("compatibility registry document must be an object");
  }
  JsonCompatibilityRegistry registry;
  registry.registry_ = json_string_or(document, "registry", "fabric-compatibility-registry");
  const Json* decisions = document.find("decisions");
  if (decisions == nullptr || !decisions->is_array()) {
    return malformed("compatibility registry document must contain a decisions array");
  }
  for (std::size_t index = 0; index < decisions->size(); ++index) {
    auto decision = compatibility_decision_from_json(decisions->at(index));
    if (!decision.ok()) {
      return decision.status();
    }
    registry.decisions_.push_back(decision.take());
  }
  return registry;
}

Result<JsonCompatibilityRegistry> JsonCompatibilityRegistry::load_file(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return Status::error(ErrorCode::IoFailure, "cannot open compatibility registry file",
                         Json::object({{"path", Json(path)}}));
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return parse(buffer.str());
}

Result<CompatibilityDecision> JsonCompatibilityRegistry::decide(const ComponentId& subject,
                                                                const SoftwareVersion& source,
                                                                const SoftwareVersion& target) const {
  for (const CompatibilityDecision& decision : decisions_) {
    if (decision.subject == subject && decision.source == source && decision.target == target) {
      return decision;
    }
  }
  return Status::error(ErrorCode::CompatibilityInsufficient,
                       "no compatibility decision covers this component and version pair",
                       Json::object({{"subject", Json(subject.str())},
                             {"source", Json(source.to_string())},
                             {"target", Json(target.to_string())}}));
}

EvidenceCheck verify_evidence(const CompatibilityDecision& declared, const CompatibilityRegistry& registry,
                              EvidencePolicy policy) {
  EvidenceCheck check;
  check.effective = declared;
  auto current = registry.decide(declared.subject, declared.source, declared.target);
  if (!current.ok()) {
    check.rationale = "compatibility registry has no decision for this pair: " + current.status().message();
    return check;
  }

  const CompatibilityDecision& fresh = current.value();
  if (fresh.evidence_digest == declared.evidence_digest) {
    check.satisfied = true;
    check.effective = fresh;
    check.rationale = "registry evidence matches the manifest digest exactly";
    return check;
  }

  if (fresh.verdict != declared.verdict) {
    check.differences.push_back("verdict " + std::string(::fabric::evolution::to_string(declared.verdict)) +
                                " -> " + std::string(::fabric::evolution::to_string(fresh.verdict)));
  }
  if (fresh.evidence_digest != declared.evidence_digest) {
    check.differences.push_back("evidence digest " + declared.evidence_digest.to_short_hex() + " -> " +
                                fresh.evidence_digest.to_short_hex());
  }

  if (policy == EvidencePolicy::RequireExactMatch) {
    check.rationale = "registry evidence has moved on and the policy requires an exact match";
    return check;
  }

  if (fresh.verdict == declared.verdict) {
    check.satisfied = true;
    check.refreshed = true;
    check.effective = fresh;
    check.rationale = "registry evidence was refreshed but the verdict is unchanged";
    return check;
  }

  check.rationale = "registry evidence was refreshed and the verdict changed";
  return check;
}

}  // namespace fabric::evolution
