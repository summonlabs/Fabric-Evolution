// Fabric Evolution — the evolution manifest.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// A manifest is the complete, sealed declaration of one controlled evolution:
// what is being evolved from and to, how long mixed-version operation may last,
// which compatibility evidence justifies it, how state is migrated, which
// protocols may be negotiated, where the irreversible boundaries are, and how
// the campaign recovers if it must stop. Nothing in the runtime may cross a
// boundary the manifest does not declare.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "fabric/evolution/compatibility.hpp"
#include "fabric/evolution/digest.hpp"
#include "fabric/evolution/features.hpp"
#include "fabric/evolution/ids.hpp"
#include "fabric/evolution/versions.hpp"

namespace fabric::evolution {

struct ComponentSpec {
  ComponentId id;
  SoftwareVersion software;
  ProtocolVersion protocol;
  SchemaVersion schema;
  FeatureSet features;
  Digest artifact_digest;
  std::string host;
  std::uint16_t port = 0;

  [[nodiscard]] bool is_valid() const noexcept;
};

// The bounded period during which predecessor and successor both exist.
struct MixedVersionWindow {
  std::uint64_t max_duration_ms = 0;
  std::uint64_t max_operations = 0;
  std::uint32_t min_participants = 1;
  bool require_read_only_shared_phase = true;
};

// One declared state-migration step. The function field names a built-in
// deterministic transformation implemented by this repository; an unknown name
// is refused rather than executed, so a manifest can never smuggle in arbitrary
// code.
struct MigrationStepSpec {
  MigrationStepId id;
  SchemaVersion from;
  SchemaVersion to;
  std::string function;
  Json parameters = Json::object();
  bool deterministic = true;
  bool reversible = true;
  bool irreversible_boundary = false;
  Digest evidence_digest;
  std::string description;
};

struct ProtocolPath {
  std::vector<ProtocolVersion> accepted;
  FeatureSet required_features;
  FeatureSet allowed_features;

  [[nodiscard]] bool accepts(const ProtocolVersion& version) const noexcept;
};

enum class RollbackKind : std::uint8_t {
  RollbackToPredecessor = 0,        // safe to return to the source version
  RollbackIfNoBoundaryCrossed = 1,  // rollback until an irreversible boundary is crossed
  ForwardRecoveryOnly = 2,          // rollback is never safe; the campaign must go forward
};

[[nodiscard]] std::string_view to_string(RollbackKind kind) noexcept;
[[nodiscard]] std::optional<RollbackKind> rollback_kind_from(std::string_view text);

struct RollbackStrategy {
  RollbackKind kind = RollbackKind::RollbackIfNoBoundaryCrossed;
  std::string recovery_plan;
  std::vector<MigrationStepId> declared_irreversible_boundaries;

  [[nodiscard]] bool declares(const MigrationStepId& step) const noexcept;
};

class EvolutionManifest {
 public:
  ManifestId id;
  CampaignId campaign;
  ShardId shard;
  Revision revision = Revision::from_value(1);
  ComponentSpec source;
  ComponentSpec target;
  MixedVersionWindow window;
  CompatibilityDecision compatibility;
  std::vector<MigrationStepSpec> migrations;
  ProtocolPath protocol;
  RollbackStrategy rollback;
  Digest digest;

  // Structural and cross-field validation. Every rule this runtime depends on is
  // checked here so that an invalid manifest cannot start a campaign.
  [[nodiscard]] Status validate() const;
  // Canonical digest over every field except the digest field itself.
  [[nodiscard]] Digest compute_digest() const;
  [[nodiscard]] Status seal();
  [[nodiscard]] Status verify_digest() const;

  // Window after intersecting the declared window with the compatibility evidence.
  [[nodiscard]] MixedVersionWindow effective_window() const;

  [[nodiscard]] std::optional<MigrationStepSpec> step_from(SchemaVersion schema) const;
  [[nodiscard]] std::optional<MigrationStepSpec> step_to(SchemaVersion schema) const;
  [[nodiscard]] std::vector<MigrationStepSpec> migration_path(SchemaVersion from, SchemaVersion to) const;
  [[nodiscard]] bool crosses_irreversible_boundary(SchemaVersion from, SchemaVersion to) const;
};

[[nodiscard]] Json component_spec_to_json(const ComponentSpec& spec);
[[nodiscard]] Result<ComponentSpec> component_spec_from_json(const Json& value);
[[nodiscard]] Json to_json(const MigrationStepSpec& step);
[[nodiscard]] Result<MigrationStepSpec> migration_step_from_json(const Json& value);
[[nodiscard]] Json to_json(const EvolutionManifest& manifest);
[[nodiscard]] Result<EvolutionManifest> manifest_from_json(const Json& value);
[[nodiscard]] Result<EvolutionManifest> manifest_from_json_text(std::string_view text);

}  // namespace fabric::evolution
