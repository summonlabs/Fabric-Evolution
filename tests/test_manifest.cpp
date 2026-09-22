// Fabric Evolution — manifest admission tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <string>
#include <vector>

#include "fabric/evolution/manifest.hpp"
#include "support/cluster.hpp"
#include "support/test_harness.hpp"

namespace fabric::evolution {
namespace {

ComponentSpec spec_of(const char* id, const char* software, const char* protocol, std::uint32_t schema) {
  return test::make_spec(id, software, protocol, schema, 41000, test::standard_features());
}

EvolutionManifest base_manifest() {
  const ComponentSpec source = spec_of("node-a", "1.0.0", "1.0", 1);
  const ComponentSpec target = spec_of("node-b", "2.0.0", "1.1", 2);
  return test::make_manifest(source, target, test::make_decision(source, target));
}

}  // namespace

FABRIC_TEST(manifest, valid_manifest_seals_and_verifies) {
  EvolutionManifest manifest = base_manifest();
  FABRIC_CHECK(manifest.digest.is_zero() == false);
  FABRIC_CHECK_OK(manifest.verify_digest());
  FABRIC_CHECK_OK(manifest.validate());
  FABRIC_CHECK_EQ(manifest.migrations.size(), static_cast<std::size_t>(1));
  const Json encoded = to_json(manifest);
  const auto decoded = manifest_from_json(encoded);
  FABRIC_CHECK_OK(decoded);
  FABRIC_CHECK_EQ(decoded.value().digest, manifest.digest);
}

FABRIC_TEST(manifest, digest_changes_when_any_field_changes) {
  EvolutionManifest manifest = base_manifest();
  const Digest original = manifest.digest;
  manifest.window.max_operations += 1;
  FABRIC_CHECK_NE(manifest.compute_digest(), original);
  EvolutionManifest other = base_manifest();
  other.rollback.recovery_plan += " ";
  FABRIC_CHECK_NE(other.compute_digest(), original);
}

FABRIC_TEST(manifest, tampered_manifest_is_rejected) {
  EvolutionManifest manifest = base_manifest();
  Json encoded = to_json(manifest);
  encoded.set("window", Json::object({{"max_duration_ms", Json(1)},
                                      {"max_operations", Json(2)},
                                      {"min_participants", Json(2)},
                                      {"require_read_only_shared_phase", Json(true)}}));
  const auto decoded = manifest_from_json(encoded);
  FABRIC_CHECK(!decoded.ok());
  FABRIC_CHECK_ERR(decoded, ErrorCode::IntegrityFailure);
}

FABRIC_TEST(manifest, unsupported_compatibility_evidence_is_refused) {
  const ComponentSpec source = spec_of("node-a", "1.0.0", "1.0", 1);
  const ComponentSpec target = spec_of("node-b", "2.0.0", "1.1", 2);
  CompatibilityDecision decision = test::make_decision(source, target, true, true);
  EvolutionManifest manifest;
  manifest.id = *ManifestId::parse("manifest-x");
  manifest.campaign = *CampaignId::parse("campaign-x");
  manifest.shard = *ShardId::parse("shard-0");
  manifest.source = source;
  manifest.target = target;
  manifest.compatibility = decision;
  manifest.window.max_operations = 10;
  manifest.window.max_duration_ms = 1000;
  manifest.protocol.accepted = {source.protocol, target.protocol};
  manifest.rollback.kind = RollbackKind::RollbackIfNoBoundaryCrossed;
  manifest.rollback.recovery_plan = "restore the predecessor";
  const Status status = manifest.validate();
  FABRIC_CHECK(!status.ok());
  FABRIC_CHECK_EQ(status.code(), ErrorCode::CompatibilityInsufficient);
}

FABRIC_TEST(manifest, migration_chain_must_be_contiguous) {
  const ComponentSpec source = spec_of("node-a", "1.0.0", "1.0", 1);
  const ComponentSpec target = spec_of("node-b", "2.0.0", "1.1", 3);
  CompatibilityDecision decision = test::make_decision(source, target);
  EvolutionManifest manifest = test::make_manifest(source, target, decision);
  // The helper builds a contiguous chain; removing the middle step breaks it.
  FABRIC_CHECK_EQ(manifest.migrations.size(), static_cast<std::size_t>(2));
  FABRIC_CHECK_OK(manifest.validate());
  manifest.migrations.erase(manifest.migrations.begin());
  FABRIC_CHECK_ERR(manifest.validate(), ErrorCode::MigrationFailure);
  FABRIC_CHECK(manifest.migration_path(source.schema, target.schema).empty());
}

FABRIC_TEST(manifest, irreversible_step_must_be_declared_before_start) {
  const ComponentSpec source = spec_of("node-a", "1.0.0", "1.0", 1);
  const ComponentSpec target = spec_of("node-b", "2.0.0", "1.1", 2);
  CompatibilityDecision decision = test::make_decision(source, target);
  EvolutionManifest manifest = test::make_manifest(source, target, decision);
  manifest.migrations.back().irreversible_boundary = true;
  manifest.migrations.back().reversible = false;
  // The rollback strategy has not been updated to declare the boundary.
  FABRIC_CHECK_ERR(manifest.validate(), ErrorCode::IrreversibleBoundary);
}

FABRIC_TEST(manifest, rollback_to_predecessor_is_refused_across_a_boundary) {
  const ComponentSpec source = spec_of("node-a", "1.0.0", "1.0", 1);
  const ComponentSpec target = spec_of("node-b", "2.0.0", "1.1", 2);
  CompatibilityDecision decision = test::make_decision(source, target);
  EvolutionManifest manifest = test::make_manifest(source, target, decision);
  manifest.migrations.back().irreversible_boundary = true;
  manifest.migrations.back().reversible = false;
  manifest.rollback.kind = RollbackKind::RollbackToPredecessor;
  manifest.rollback.declared_irreversible_boundaries.push_back(manifest.migrations.back().id);
  FABRIC_CHECK_ERR(manifest.validate(), ErrorCode::IrreversibleBoundary);
}

FABRIC_TEST(manifest, forward_recovery_only_requires_a_crossed_boundary_and_a_plan) {
  const ComponentSpec source = spec_of("node-a", "1.0.0", "1.0", 1);
  const ComponentSpec target = spec_of("node-b", "2.0.0", "1.1", 2);
  CompatibilityDecision decision = test::make_decision(source, target);
  EvolutionManifest manifest = test::make_manifest(source, target, decision);
  manifest.rollback.kind = RollbackKind::ForwardRecoveryOnly;
  manifest.rollback.recovery_plan = "go forward";
  FABRIC_CHECK_ERR(manifest.validate(), ErrorCode::Malformed);

  EvolutionManifest declared = test::make_manifest(source, target, decision, "campaign-1", true);
  FABRIC_CHECK_OK(declared.validate());
  declared.rollback.recovery_plan.clear();
  FABRIC_CHECK_ERR(declared.validate(), ErrorCode::Malformed);
}

FABRIC_TEST(manifest, uncertified_feature_is_refused) {
  ComponentSpec source = spec_of("node-a", "1.0.0", "1.0", 1);
  ComponentSpec target = spec_of("node-b", "2.0.0", "1.1", 2);
  source.features.add(Feature::PipelinedCatchUp);
  target.features.add(Feature::PipelinedCatchUp);
  CompatibilityDecision decision = test::make_decision(source, target);
  decision.certified_features = decision.certified_features.without(FeatureSet::of(Feature::PipelinedCatchUp));
  FABRIC_CHECK_OK(decision.seal());
  EvolutionManifest manifest = test::make_manifest(source, target, decision);
  manifest.protocol.allowed_features.add(Feature::PipelinedCatchUp);
  FABRIC_CHECK_ERR(manifest.validate(), ErrorCode::CompatibilityInsufficient);
}

FABRIC_TEST(manifest, feature_not_supported_by_a_participant_is_refused) {
  const ComponentSpec source = spec_of("node-a", "1.0.0", "1.0", 1);
  const ComponentSpec target = spec_of("node-b", "2.0.0", "1.1", 2);
  CompatibilityDecision decision = test::make_decision(source, target);
  EvolutionManifest manifest = test::make_manifest(source, target, decision);
  FABRIC_CHECK_OK(manifest.validate());
  // The target build does not offer incremental catch-up, which the manifest
  // requires from every participant.
  manifest.target.features =
      manifest.target.features.without(FeatureSet::of(Feature::IncrementalCatchUp));
  FABRIC_CHECK_ERR(manifest.validate(), ErrorCode::FeatureNotNegotiated);
}

FABRIC_TEST(manifest, protocol_path_must_cover_both_ends) {
  const ComponentSpec source = spec_of("node-a", "1.0.0", "1.0", 1);
  const ComponentSpec target = spec_of("node-b", "2.0.0", "1.1", 2);
  CompatibilityDecision decision = test::make_decision(source, target);
  EvolutionManifest manifest = test::make_manifest(source, target, decision);
  manifest.protocol.accepted = {ProtocolVersion::of(1, 0)};
  FABRIC_CHECK_ERR(manifest.validate(), ErrorCode::UnsupportedProtocol);
  manifest.protocol.accepted = {ProtocolVersion::of(1, 0), ProtocolVersion::of(2, 0)};
  FABRIC_CHECK_ERR(manifest.validate(), ErrorCode::UnsupportedProtocol);
}

FABRIC_TEST(manifest, window_bounds_are_enforced) {
  EvolutionManifest manifest = base_manifest();
  manifest.window.max_operations = 0;
  FABRIC_CHECK_ERR(manifest.validate(), ErrorCode::InvalidArgument);
  manifest = base_manifest();
  manifest.window.max_duration_ms = 0;
  FABRIC_CHECK_ERR(manifest.validate(), ErrorCode::InvalidArgument);
  manifest = base_manifest();
  manifest.window.max_duration_ms = 400ULL * 24ULL * 60ULL * 60ULL * 1000ULL;
  FABRIC_CHECK_ERR(manifest.validate(), ErrorCode::BoundsExceeded);
  manifest = base_manifest();
  manifest.window.min_participants = 0;
  FABRIC_CHECK_ERR(manifest.validate(), ErrorCode::InvalidArgument);
}

FABRIC_TEST(manifest, effective_window_is_intersected_with_the_registry_limits) {
  const ComponentSpec source = spec_of("node-a", "1.0.0", "1.0", 1);
  const ComponentSpec target = spec_of("node-b", "2.0.0", "1.1", 2);
  CompatibilityDecision decision = test::make_decision(source, target);
  decision.max_mixed_version_operations = 50;
  decision.max_mixed_version_ms = 100;
  FABRIC_CHECK_OK(decision.seal());
  EvolutionManifest manifest = test::make_manifest(source, target, decision);
  const MixedVersionWindow effective = manifest.effective_window();
  FABRIC_CHECK_EQ(effective.max_operations, static_cast<std::uint64_t>(50));
  FABRIC_CHECK_EQ(effective.max_duration_ms, static_cast<std::uint64_t>(100));
}

FABRIC_TEST(manifest, malformed_documents_are_rejected) {
  FABRIC_CHECK_ERR(manifest_from_json(Json::array()), ErrorCode::Malformed);
  FABRIC_CHECK_ERR(manifest_from_json(Json::object()), ErrorCode::Malformed);
  FABRIC_CHECK_ERR(manifest_from_json_text("not json"), ErrorCode::Malformed);
  Json partial = to_json(base_manifest());
  partial.erase("migrations");
  FABRIC_CHECK_ERR(manifest_from_json(partial), ErrorCode::Malformed);
  Json no_rollback = to_json(base_manifest());
  no_rollback.erase("rollback");
  FABRIC_CHECK_ERR(manifest_from_json(no_rollback), ErrorCode::Malformed);
  Json bad_step = to_json(base_manifest());
  bad_step.set("migrations", Json::array({Json::object({{"id", Json("s")}})}));
  FABRIC_CHECK_ERR(manifest_from_json(bad_step), ErrorCode::Malformed);
}

}  // namespace fabric::evolution
