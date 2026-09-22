// Fabric Evolution — the evolution manifest (implementation).
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric/evolution/manifest.hpp"

#include <algorithm>
#include <set>

namespace fabric::evolution {
namespace {

constexpr std::uint64_t kMaxMixedVersionWindowMs = 30ULL * 24ULL * 60ULL * 60ULL * 1000ULL;
constexpr std::uint64_t kMaxMixedVersionOperations = 10'000'000'000ULL;
constexpr std::size_t kMaxMigrationSteps = 128;

[[nodiscard]] Json feature_array(const FeatureSet& features) {
  Json out = Json::array();
  for (const std::string& name : features.names()) {
    out.push_back(Json(name));
  }
  return out;
}

[[nodiscard]] Result<FeatureSet> features_from(const Json& value, const char* field) {
  FeatureSet set;
  if (value.is_null()) {
    return set;
  }
  if (!value.is_array()) {
    return malformed(std::string(field) + " must be an array of feature names");
  }
  std::vector<std::string> names;
  for (std::size_t index = 0; index < value.size(); ++index) {
    const std::string* name = value.at(index).try_string();
    if (name == nullptr) {
      return malformed(std::string(field) + " entries must be strings");
    }
    names.push_back(*name);
  }
  std::string error;
  const auto parsed = FeatureSet::from_names(names, error);
  if (!parsed.has_value()) {
    return malformed(error);
  }
  return *parsed;
}

}  // namespace

bool ComponentSpec::is_valid() const noexcept {
  return id.is_valid() && software.is_valid() && protocol.is_valid() && schema.is_valid() &&
         !host.empty() && port != 0;
}

bool ProtocolPath::accepts(const ProtocolVersion& version) const noexcept {
  return std::find(accepted.begin(), accepted.end(), version) != accepted.end();
}

std::string_view to_string(RollbackKind kind) noexcept {
  switch (kind) {
    case RollbackKind::RollbackToPredecessor:
      return "rollback_to_predecessor";
    case RollbackKind::RollbackIfNoBoundaryCrossed:
      return "rollback_if_no_boundary_crossed";
    case RollbackKind::ForwardRecoveryOnly:
      return "forward_recovery_only";
  }
  return "unknown";
}

std::optional<RollbackKind> rollback_kind_from(std::string_view text) {
  if (text == "rollback_to_predecessor") {
    return RollbackKind::RollbackToPredecessor;
  }
  if (text == "rollback_if_no_boundary_crossed") {
    return RollbackKind::RollbackIfNoBoundaryCrossed;
  }
  if (text == "forward_recovery_only") {
    return RollbackKind::ForwardRecoveryOnly;
  }
  return std::nullopt;
}

bool RollbackStrategy::declares(const MigrationStepId& step) const noexcept {
  return std::find(declared_irreversible_boundaries.begin(), declared_irreversible_boundaries.end(),
                   step) != declared_irreversible_boundaries.end();
}

MixedVersionWindow EvolutionManifest::effective_window() const {
  MixedVersionWindow effective = window;
  if (compatibility.max_mixed_version_ms != 0) {
    effective.max_duration_ms = std::min(effective.max_duration_ms, compatibility.max_mixed_version_ms);
  }
  if (compatibility.max_mixed_version_operations != 0) {
    effective.max_operations =
        std::min(effective.max_operations, compatibility.max_mixed_version_operations);
  }
  return effective;
}

std::optional<MigrationStepSpec> EvolutionManifest::step_from(SchemaVersion schema) const {
  for (const MigrationStepSpec& step : migrations) {
    if (step.from == schema) {
      return step;
    }
  }
  return std::nullopt;
}

std::optional<MigrationStepSpec> EvolutionManifest::step_to(SchemaVersion schema) const {
  for (const MigrationStepSpec& step : migrations) {
    if (step.to == schema) {
      return step;
    }
  }
  return std::nullopt;
}

std::vector<MigrationStepSpec> EvolutionManifest::migration_path(SchemaVersion from,
                                                                 SchemaVersion to) const {
  std::vector<MigrationStepSpec> path;
  if (from == to) {
    return path;
  }
  if (to < from) {
    return path;
  }
  SchemaVersion current = from;
  std::size_t guard = 0;
  while (current != to && guard <= migrations.size()) {
    const auto step = step_from(current);
    if (!step.has_value()) {
      return {};
    }
    if (step->to <= current) {
      return {};
    }
    path.push_back(*step);
    current = step->to;
    ++guard;
  }
  if (current != to) {
    return {};
  }
  return path;
}

bool EvolutionManifest::crosses_irreversible_boundary(SchemaVersion from, SchemaVersion to) const {
  for (const MigrationStepSpec& step : migration_path(from, to)) {
    if (step.irreversible_boundary) {
      return true;
    }
  }
  return false;
}

Status EvolutionManifest::validate() const {
  if (!id.is_valid()) {
    return invalid_argument("manifest id is required");
  }
  if (!campaign.is_valid()) {
    return invalid_argument("manifest campaign id is required");
  }
  if (!shard.is_valid()) {
    return invalid_argument("manifest shard id is required");
  }
  if (!source.is_valid()) {
    return invalid_argument("manifest source component spec is incomplete");
  }
  if (!target.is_valid()) {
    return invalid_argument("manifest target component spec is incomplete");
  }
  if (source.id == target.id && source.host == target.host && source.port == target.port) {
    return invalid_argument("manifest source and target endpoints are identical");
  }
  if (migrations.size() > kMaxMigrationSteps) {
    return Status::error(ErrorCode::BoundsExceeded, "manifest declares too many migration steps",
                         Json::object({{"steps", Json(static_cast<std::uint64_t>(migrations.size()))},
                               {"limit", Json(static_cast<std::uint64_t>(kMaxMigrationSteps))}}));
  }

  // ---- compatibility evidence -------------------------------------------
  const Status evidence_status = compatibility.verify_digest();
  if (!evidence_status.ok()) {
    return evidence_status;
  }
  if (compatibility.verdict == CompatibilityVerdict::Unsupported) {
    return Status::error(ErrorCode::CompatibilityInsufficient,
                         "compatibility registry marks this version pair unsupported",
                         Json::object({{"decision_id", Json(compatibility.decision_id)}}));
  }
  if (compatibility.subject != source.id) {
    return Status::error(ErrorCode::CompatibilityInsufficient,
                         "compatibility decision subject does not match the manifest source component",
                         Json::object({{"subject", Json(compatibility.subject.str())},
                               {"source", Json(source.id.str())}}));
  }
  if (compatibility.source != source.software || compatibility.target != target.software) {
    return Status::error(ErrorCode::CompatibilityInsufficient,
                         "compatibility decision does not cover the manifest software versions",
                         Json::object({{"decision_source", Json(compatibility.source.to_string())},
                               {"decision_target", Json(compatibility.target.to_string())},
                               {"manifest_source", Json(source.software.to_string())},
                               {"manifest_target", Json(target.software.to_string())}}));
  }
  if (compatibility.source_protocol != source.protocol ||
      compatibility.target_protocol != target.protocol) {
    return Status::error(ErrorCode::CompatibilityInsufficient,
                         "compatibility decision does not cover the manifest protocol versions");
  }
  if (compatibility.source_schema != source.schema || compatibility.target_schema != target.schema) {
    return Status::error(ErrorCode::CompatibilityInsufficient,
                         "compatibility decision does not cover the manifest schema versions");
  }

  // ---- migration chain ---------------------------------------------------
  std::set<std::string> step_ids;
  for (const MigrationStepSpec& step : migrations) {
    if (!step.id.is_valid()) {
      return invalid_argument("migration step id is required");
    }
    if (!step_ids.insert(step.id.str()).second) {
      return invalid_argument("duplicate migration step id: " + step.id.str());
    }
    if (step.function.empty()) {
      return invalid_argument("migration step " + step.id.str() + " does not name a function");
    }
    if (!step.from.is_valid() || !step.to.is_valid() || step.to <= step.from) {
      return Status::error(ErrorCode::Malformed,
                           "migration step " + step.id.str() + " must advance the schema version",
                           Json::object({{"from", Json(step.from.value())}, {"to", Json(step.to.value())}}));
    }
    if (step.irreversible_boundary && step.reversible) {
      return Status::error(ErrorCode::Malformed,
                           "migration step " + step.id.str() +
                               " is declared both irreversible and reversible");
    }
    if (!step.deterministic) {
      return Status::error(ErrorCode::Malformed,
                           "migration step " + step.id.str() + " is not deterministic",
                           Json::object({{"step", Json(step.id.str())}}));
    }
    // Every irreversible step must be declared as a boundary before the
    // campaign is allowed to start.
    if (step.irreversible_boundary && !rollback.declares(step.id)) {
      return Status::error(
          ErrorCode::IrreversibleBoundary,
          "migration step " + step.id.str() +
              " is irreversible but the rollback strategy does not declare the boundary",
          Json::object({{"step", Json(step.id.str())}}));
    }
  }
  if (source.schema != target.schema) {
    const std::vector<MigrationStepSpec> path = migration_path(source.schema, target.schema);
    if (path.empty()) {
      return Status::error(
          ErrorCode::MigrationFailure,
          "manifest does not declare a contiguous migration path from source to target schema",
          Json::object({{"from", Json(source.schema.value())}, {"to", Json(target.schema.value())}}));
    }
  } else if (!migrations.empty()) {
    return Status::error(ErrorCode::Malformed,
                         "manifest declares migration steps but source and target schema are equal");
  }

  // A forward-recovery-only campaign must declare why rollback is unsafe.
  if (rollback.kind == RollbackKind::ForwardRecoveryOnly && rollback.recovery_plan.empty()) {
    return Status::error(ErrorCode::Malformed,
                         "forward-recovery-only strategy must describe the recovery plan");
  }
  if (rollback.kind == RollbackKind::ForwardRecoveryOnly &&
      !crosses_irreversible_boundary(source.schema, target.schema)) {
    return Status::error(
        ErrorCode::Malformed,
        "forward-recovery-only strategy is only permitted when an irreversible boundary is crossed");
  }
  if (rollback.kind == RollbackKind::RollbackToPredecessor &&
      crosses_irreversible_boundary(source.schema, target.schema)) {
    return Status::error(ErrorCode::IrreversibleBoundary,
                         "rollback-to-predecessor is declared but the migration path crosses an "
                         "irreversible boundary");
  }
  for (const MigrationStepId& boundary : rollback.declared_irreversible_boundaries) {
    bool found = false;
    for (const MigrationStepSpec& step : migrations) {
      if (step.id == boundary && step.irreversible_boundary) {
        found = true;
        break;
      }
    }
    if (!found) {
      return Status::error(ErrorCode::IrreversibleBoundary,
                           "rollback strategy declares a boundary that no migration step marks irreversib"
                           "le: " + boundary.str());
    }
  }

  // ---- protocol path -----------------------------------------------------
  if (protocol.accepted.empty()) {
    return invalid_argument("manifest must declare at least one accepted protocol version");
  }
  if (!protocol.accepts(source.protocol)) {
    return Status::error(ErrorCode::UnsupportedProtocol,
                         "manifest protocol path does not accept the source protocol version");
  }
  if (!protocol.accepts(target.protocol)) {
    return Status::error(ErrorCode::UnsupportedProtocol,
                         "manifest protocol path does not accept the target protocol version");
  }
  for (const ProtocolVersion& version : protocol.accepted) {
    if (!version.compatible_with(source.protocol) || !version.compatible_with(target.protocol)) {
      return Status::error(ErrorCode::UnsupportedProtocol,
                           "manifest protocol path contains a version that is not major-compatible");
    }
  }
  if (!protocol.required_features.contains_all(protocol.required_features)) {
    return invalid_argument("required feature set is self-inconsistent");
  }
  if (!protocol.allowed_features.contains_all(protocol.required_features)) {
    return Status::error(ErrorCode::FeatureNotNegotiated,
                         "manifest requires features it does not allow");
  }
  const FeatureSet participant_support = source.features.intersect(target.features);
  if (!participant_support.contains_all(protocol.required_features)) {
    const FeatureSet missing = protocol.required_features.without(participant_support);
    Json names = Json::array();
    for (const std::string& name : missing.names()) {
      names.push_back(Json(name));
    }
    return Status::error(
        ErrorCode::FeatureNotNegotiated,
        "manifest requires features that at least one participant does not support", std::move(names));
  }
  if (!compatibility.certified_features.contains_all(protocol.required_features)) {
    const FeatureSet uncertified = protocol.required_features.without(compatibility.certified_features);
    Json names = Json::array();
    for (const std::string& name : uncertified.names()) {
      names.push_back(Json(name));
    }
    return Status::error(ErrorCode::CompatibilityInsufficient,
                         "manifest requires features the compatibility registry has not certified",
                         std::move(names));
  }
  const FeatureSet uncertified_allowed = protocol.allowed_features.without(compatibility.certified_features);
  if (!uncertified_allowed.empty()) {
    Json names = Json::array();
    for (const std::string& name : uncertified_allowed.names()) {
      names.push_back(Json(name));
    }
    return Status::error(ErrorCode::CompatibilityInsufficient,
                         "manifest allows features the compatibility registry has not certified",
                         std::move(names));
  }

  // ---- mixed-version window ---------------------------------------------
  if (window.max_duration_ms == 0) {
    return invalid_argument("manifest mixed-version window must have a positive duration bound");
  }
  if (window.max_duration_ms > kMaxMixedVersionWindowMs) {
    return Status::error(ErrorCode::BoundsExceeded, "manifest mixed-version window is unreasonably long",
                         Json::object({{"max_duration_ms", Json(window.max_duration_ms)},
                               {"limit_ms", Json(kMaxMixedVersionWindowMs)}}));
  }
  if (window.max_operations == 0) {
    return invalid_argument("manifest mixed-version window must have a positive operation bound");
  }
  if (window.max_operations > kMaxMixedVersionOperations) {
    return Status::error(ErrorCode::BoundsExceeded, "manifest mixed-version operation bound is too large");
  }
  if (window.min_participants == 0) {
    return invalid_argument("manifest mixed-version window must require at least one participant");
  }
  if (compatibility.verdict == CompatibilityVerdict::Supported && !compatibility.rollback_permitted &&
      rollback.kind == RollbackKind::RollbackToPredecessor) {
    return Status::error(ErrorCode::CompatibilityInsufficient,
                         "compatibility registry does not permit rollback for this version pair");
  }
  if (rollback.kind != RollbackKind::ForwardRecoveryOnly && rollback.recovery_plan.empty()) {
    return invalid_argument("rollback strategy must describe its recovery plan");
  }
  return Status::success();
}

Digest EvolutionManifest::compute_digest() const {
  Json out = Json::object();
  out.set("id", Json(id.str()));
  out.set("campaign", Json(campaign.str()));
  out.set("shard", Json(shard.str()));
  out.set("revision", Json(revision.value()));
  out.set("source", component_spec_to_json(source));
  out.set("target", component_spec_to_json(target));
  out.set("compatibility", to_json(compatibility));
  Json window_json = Json::object();
  window_json.set("max_duration_ms", Json(window.max_duration_ms));
  window_json.set("max_operations", Json(window.max_operations));
  window_json.set("min_participants", Json(window.min_participants));
  window_json.set("require_read_only_shared_phase", Json(window.require_read_only_shared_phase));
  out.set("window", std::move(window_json));
  Json steps = Json::array();
  for (const MigrationStepSpec& step : migrations) {
    steps.push_back(to_json(step));
  }
  out.set("migrations", std::move(steps));
  Json protocol_json = Json::object();
  Json accepted = Json::array();
  for (const ProtocolVersion& version : protocol.accepted) {
    accepted.push_back(Json(version.to_string()));
  }
  protocol_json.set("accepted", std::move(accepted));
  protocol_json.set("required_features", feature_array(protocol.required_features));
  protocol_json.set("allowed_features", feature_array(protocol.allowed_features));
  out.set("protocol", std::move(protocol_json));
  Json rollback_json = Json::object();
  rollback_json.set("kind", Json(std::string(::fabric::evolution::to_string(rollback.kind))));
  rollback_json.set("recovery_plan", Json(rollback.recovery_plan));
  Json boundaries = Json::array();
  for (const MigrationStepId& boundary : rollback.declared_irreversible_boundaries) {
    boundaries.push_back(Json(boundary.str()));
  }
  rollback_json.set("declared_irreversible_boundaries", std::move(boundaries));
  out.set("rollback", std::move(rollback_json));
  return sha256(out.dump());
}

Status EvolutionManifest::seal() {
  const Status validation = validate();
  if (!validation.ok()) {
    return validation;
  }
  digest = compute_digest();
  return Status::success();
}

Status EvolutionManifest::verify_digest() const {
  const Digest recomputed = compute_digest();
  if (recomputed != digest) {
    return Status::error(ErrorCode::IntegrityFailure, "manifest digest mismatch",
                         Json::object({{"recorded", Json(digest.to_hex())},
                               {"recomputed", Json(recomputed.to_hex())}}));
  }
  return Status::success();
}

Json component_spec_to_json(const ComponentSpec& spec) {
  Json out = Json::object();
  out.set("id", Json(spec.id.str()));
  out.set("software", Json(spec.software.to_string()));
  out.set("protocol", Json(spec.protocol.to_string()));
  out.set("schema", Json(spec.schema.value()));
  out.set("features", feature_array(spec.features));
  out.set("artifact_digest", Json(spec.artifact_digest.to_hex()));
  out.set("host", Json(spec.host));
  out.set("port", Json(static_cast<std::uint64_t>(spec.port)));
  return out;
}

Result<ComponentSpec> component_spec_from_json(const Json& value) {
  if (!value.is_object()) {
    return malformed("component spec must be an object");
  }
  ComponentSpec spec;
  const auto id = json_string(value, "id");
  if (!id.has_value()) {
    return malformed("component spec is missing id");
  }
  auto parsed_id = ComponentId::parse(*id);
  if (!parsed_id.has_value()) {
    return malformed("component spec id is invalid: " + *id);
  }
  spec.id = *parsed_id;

  const auto software = json_string(value, "software");
  if (!software.has_value()) {
    return malformed("component spec is missing software version");
  }
  auto parsed_software = SoftwareVersion::parse(*software);
  if (!parsed_software.has_value()) {
    return malformed("component spec software version is invalid: " + *software);
  }
  spec.software = *parsed_software;

  const auto protocol = json_string(value, "protocol");
  if (!protocol.has_value()) {
    return malformed("component spec is missing protocol version");
  }
  auto parsed_protocol = ProtocolVersion::parse(*protocol);
  if (!parsed_protocol.has_value()) {
    return malformed("component spec protocol version is invalid: " + *protocol);
  }
  spec.protocol = *parsed_protocol;

  const auto schema = json_u64(value, "schema");
  if (!schema.has_value() || *schema == 0 || *schema > UINT32_MAX) {
    return malformed("component spec schema version is missing or out of range");
  }
  spec.schema = SchemaVersion::from_value(static_cast<std::uint32_t>(*schema));

  const Json* features = value.find("features");
  if (features != nullptr) {
    auto parsed_features = features_from(*features, "component spec features");
    if (!parsed_features.ok()) {
      return parsed_features.status();
    }
    spec.features = parsed_features.value();
  }

  const auto artifact = json_string(value, "artifact_digest");
  if (artifact.has_value()) {
    auto parsed_artifact = Digest::from_hex(*artifact);
    if (!parsed_artifact.has_value()) {
      return malformed("component spec artifact_digest is not a sha256 hex digest");
    }
    spec.artifact_digest = *parsed_artifact;
  }

  const auto host = json_string(value, "host");
  if (!host.has_value() || host->empty()) {
    return malformed("component spec is missing host");
  }
  spec.host = *host;

  const auto port = json_u64(value, "port");
  if (!port.has_value() || *port == 0 || *port > 65535) {
    return malformed("component spec port is missing or out of range");
  }
  spec.port = static_cast<std::uint16_t>(*port);
  return spec;
}

Json to_json(const MigrationStepSpec& step) {
  Json out = Json::object();
  out.set("id", Json(step.id.str()));
  out.set("from", Json(step.from.value()));
  out.set("to", Json(step.to.value()));
  out.set("function", Json(step.function));
  out.set("parameters", step.parameters);
  out.set("deterministic", Json(step.deterministic));
  out.set("reversible", Json(step.reversible));
  out.set("irreversible_boundary", Json(step.irreversible_boundary));
  out.set("evidence_digest", Json(step.evidence_digest.to_hex()));
  out.set("description", Json(step.description));
  return out;
}

Result<MigrationStepSpec> migration_step_from_json(const Json& value) {
  if (!value.is_object()) {
    return malformed("migration step must be an object");
  }
  MigrationStepSpec step;
  const auto id = json_string(value, "id");
  if (!id.has_value()) {
    return malformed("migration step is missing id");
  }
  auto parsed_id = MigrationStepId::parse(*id);
  if (!parsed_id.has_value()) {
    return malformed("migration step id is invalid: " + *id);
  }
  step.id = *parsed_id;

  const auto from = json_u64(value, "from");
  const auto to = json_u64(value, "to");
  if (!from.has_value() || !to.has_value() || *from == 0 || *to == 0 || *from > UINT32_MAX ||
      *to > UINT32_MAX) {
    return malformed("migration step schema versions are missing or out of range");
  }
  step.from = SchemaVersion::from_value(static_cast<std::uint32_t>(*from));
  step.to = SchemaVersion::from_value(static_cast<std::uint32_t>(*to));

  const auto function = json_string(value, "function");
  if (!function.has_value() || function->empty()) {
    return malformed("migration step is missing function");
  }
  step.function = *function;

  if (const Json* parameters = value.find("parameters")) {
    if (!parameters->is_object()) {
      return malformed("migration step parameters must be an object");
    }
    step.parameters = *parameters;
  }
  step.deterministic = json_bool_or(value, "deterministic", true);
  step.reversible = json_bool_or(value, "reversible", true);
  step.irreversible_boundary = json_bool_or(value, "irreversible_boundary", false);
  step.description = json_string_or(value, "description", "");
  const auto evidence = json_string(value, "evidence_digest");
  if (evidence.has_value()) {
    auto parsed_evidence = Digest::from_hex(*evidence);
    if (!parsed_evidence.has_value()) {
      return malformed("migration step evidence_digest is not a sha256 hex digest");
    }
    step.evidence_digest = *parsed_evidence;
  }
  return step;
}

Json to_json(const EvolutionManifest& manifest) {
  Json out = Json::object();
  out.set("id", Json(manifest.id.str()));
  out.set("campaign", Json(manifest.campaign.str()));
  out.set("shard", Json(manifest.shard.str()));
  out.set("revision", Json(manifest.revision.value()));
  out.set("source", component_spec_to_json(manifest.source));
  out.set("target", component_spec_to_json(manifest.target));
  out.set("compatibility", to_json(manifest.compatibility));
  Json window_json = Json::object();
  window_json.set("max_duration_ms", Json(manifest.window.max_duration_ms));
  window_json.set("max_operations", Json(manifest.window.max_operations));
  window_json.set("min_participants", Json(manifest.window.min_participants));
  window_json.set("require_read_only_shared_phase", Json(manifest.window.require_read_only_shared_phase));
  out.set("window", std::move(window_json));
  Json steps = Json::array();
  for (const MigrationStepSpec& step : manifest.migrations) {
    steps.push_back(to_json(step));
  }
  out.set("migrations", std::move(steps));
  Json protocol_json = Json::object();
  Json accepted = Json::array();
  for (const ProtocolVersion& version : manifest.protocol.accepted) {
    accepted.push_back(Json(version.to_string()));
  }
  protocol_json.set("accepted", std::move(accepted));
  protocol_json.set("required_features", feature_array(manifest.protocol.required_features));
  protocol_json.set("allowed_features", feature_array(manifest.protocol.allowed_features));
  out.set("protocol", std::move(protocol_json));
  Json rollback_json = Json::object();
  rollback_json.set("kind", Json(std::string(::fabric::evolution::to_string(manifest.rollback.kind))));
  rollback_json.set("recovery_plan", Json(manifest.rollback.recovery_plan));
  Json boundaries = Json::array();
  for (const MigrationStepId& boundary : manifest.rollback.declared_irreversible_boundaries) {
    boundaries.push_back(Json(boundary.str()));
  }
  rollback_json.set("declared_irreversible_boundaries", std::move(boundaries));
  out.set("rollback", std::move(rollback_json));
  out.set("digest", Json(manifest.digest.to_hex()));
  return out;
}

Result<EvolutionManifest> manifest_from_json(const Json& value) {
  if (!value.is_object()) {
    return malformed("manifest must be an object");
  }
  EvolutionManifest manifest;
  const auto id = json_string(value, "id");
  if (!id.has_value()) {
    return malformed("manifest is missing id");
  }
  auto parsed_id = ManifestId::parse(*id);
  if (!parsed_id.has_value()) {
    return malformed("manifest id is invalid: " + *id);
  }
  manifest.id = *parsed_id;

  const auto campaign = json_string(value, "campaign");
  if (!campaign.has_value()) {
    return malformed("manifest is missing campaign");
  }
  auto parsed_campaign = CampaignId::parse(*campaign);
  if (!parsed_campaign.has_value()) {
    return malformed("manifest campaign id is invalid: " + *campaign);
  }
  manifest.campaign = *parsed_campaign;

  const auto shard = json_string(value, "shard");
  if (!shard.has_value()) {
    return malformed("manifest is missing shard");
  }
  auto parsed_shard = ShardId::parse(*shard);
  if (!parsed_shard.has_value()) {
    return malformed("manifest shard id is invalid: " + *shard);
  }
  manifest.shard = *parsed_shard;

  manifest.revision = Revision::from_value(json_u64_or(value, "revision", 1));

  const Json* source = value.find("source");
  const Json* target = value.find("target");
  if (source == nullptr || target == nullptr) {
    return malformed("manifest must declare source and target component specs");
  }
  auto parsed_source = component_spec_from_json(*source);
  if (!parsed_source.ok()) {
    return parsed_source.status();
  }
  manifest.source = parsed_source.value();
  auto parsed_target = component_spec_from_json(*target);
  if (!parsed_target.ok()) {
    return parsed_target.status();
  }
  manifest.target = parsed_target.value();

  const Json* compatibility = value.find("compatibility");
  if (compatibility == nullptr) {
    return malformed("manifest must embed a compatibility decision");
  }
  auto parsed_compatibility = compatibility_decision_from_json(*compatibility);
  if (!parsed_compatibility.ok()) {
    return parsed_compatibility.status();
  }
  manifest.compatibility = parsed_compatibility.value();

  const Json* window = value.find("window");
  if (window == nullptr || !window->is_object()) {
    return malformed("manifest must declare a mixed-version window");
  }
  manifest.window.max_duration_ms = json_u64_or(*window, "max_duration_ms", 0);
  manifest.window.max_operations = json_u64_or(*window, "max_operations", 0);
  const auto participants = json_u64(*window, "min_participants");
  if (!participants.has_value() || *participants == 0 || *participants > UINT32_MAX) {
    return malformed("manifest min_participants is missing or out of range");
  }
  manifest.window.min_participants = static_cast<std::uint32_t>(*participants);
  manifest.window.require_read_only_shared_phase =
      json_bool_or(*window, "require_read_only_shared_phase", true);

  const Json* migrations = value.find("migrations");
  if (migrations == nullptr || !migrations->is_array()) {
    return malformed("manifest must declare a migrations array");
  }
  for (std::size_t index = 0; index < migrations->size(); ++index) {
    auto step = migration_step_from_json(migrations->at(index));
    if (!step.ok()) {
      return step.status();
    }
    manifest.migrations.push_back(step.take());
  }

  const Json* protocol = value.find("protocol");
  if (protocol == nullptr || !protocol->is_object()) {
    return malformed("manifest must declare a protocol path");
  }
  const Json* accepted = protocol->find("accepted");
  if (accepted == nullptr || !accepted->is_array() || accepted->size() == 0) {
    return malformed("manifest protocol path must list accepted versions");
  }
  for (std::size_t index = 0; index < accepted->size(); ++index) {
    const std::string* text = accepted->at(index).try_string();
    if (text == nullptr) {
      return malformed("manifest accepted protocol versions must be strings");
    }
    auto version = ProtocolVersion::parse(*text);
    if (!version.has_value()) {
      return malformed("manifest accepted protocol version is invalid: " + *text);
    }
    manifest.protocol.accepted.push_back(*version);
  }
  if (const Json* required = protocol->find("required_features")) {
    auto parsed = features_from(*required, "protocol required_features");
    if (!parsed.ok()) {
      return parsed.status();
    }
    manifest.protocol.required_features = parsed.value();
  }
  if (const Json* allowed = protocol->find("allowed_features")) {
    auto parsed = features_from(*allowed, "protocol allowed_features");
    if (!parsed.ok()) {
      return parsed.status();
    }
    manifest.protocol.allowed_features = parsed.value();
  }

  const Json* rollback = value.find("rollback");
  if (rollback == nullptr || !rollback->is_object()) {
    return malformed("manifest must declare a rollback strategy");
  }
  const auto kind = json_string(*rollback, "kind");
  if (!kind.has_value()) {
    return malformed("rollback strategy is missing kind");
  }
  const auto parsed_kind = rollback_kind_from(*kind);
  if (!parsed_kind.has_value()) {
    return malformed("rollback strategy kind is invalid: " + *kind);
  }
  manifest.rollback.kind = *parsed_kind;
  manifest.rollback.recovery_plan = json_string_or(*rollback, "recovery_plan", "");
  if (const Json* boundaries = rollback->find("declared_irreversible_boundaries")) {
    if (!boundaries->is_array()) {
      return malformed("rollback declared_irreversible_boundaries must be an array");
    }
    for (std::size_t index = 0; index < boundaries->size(); ++index) {
      const std::string* text = boundaries->at(index).try_string();
      if (text == nullptr) {
        return malformed("rollback boundaries must be strings");
      }
      auto boundary = MigrationStepId::parse(*text);
      if (!boundary.has_value()) {
        return malformed("rollback boundary id is invalid: " + *text);
      }
      manifest.rollback.declared_irreversible_boundaries.push_back(*boundary);
    }
  }

  const auto digest_text = json_string(value, "digest");
  if (digest_text.has_value()) {
    auto parsed_digest = Digest::from_hex(*digest_text);
    if (!parsed_digest.has_value()) {
      return malformed("manifest digest is not a sha256 hex digest");
    }
    manifest.digest = *parsed_digest;
    const Status verified = manifest.verify_digest();
    if (!verified.ok()) {
      return verified;
    }
  } else {
    const Status sealed = manifest.seal();
    if (!sealed.ok()) {
      return sealed;
    }
  }
  return manifest;
}

Result<EvolutionManifest> manifest_from_json_text(std::string_view text) {
  const JsonParseResult parsed = parse_json(text);
  if (!parsed.ok()) {
    return malformed("manifest is not valid JSON: " + parsed.error);
  }
  return manifest_from_json(*parsed.value);
}

}  // namespace fabric::evolution
