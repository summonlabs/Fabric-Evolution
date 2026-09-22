// Fabric Evolution — planning and migrating state, without a cluster.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// This example walks the pure decision surface of the runtime: it builds a
// compatibility decision, admits an evolution manifest, inspects the sealed
// digest, applies the declared state migration and shows that the runtime
// refuses to downgrade across an irreversible boundary.

#include <cstdio>
#include <string>

#include "fabric/evolution/compatibility.hpp"
#include "fabric/evolution/manifest.hpp"
#include "fabric/evolution/migration.hpp"

namespace {

using namespace fabric::evolution;

ComponentSpec spec_of(const char* id, const char* software, const char* protocol, std::uint32_t schema) {
  ComponentSpec spec;
  spec.id = *ComponentId::parse(id);
  spec.software = *SoftwareVersion::parse(software);
  spec.protocol = *ProtocolVersion::parse(protocol);
  spec.schema = SchemaVersion::from_value(schema);
  spec.features.add(Feature::SnapshotTransfer)
      .add(Feature::IncrementalCatchUp)
      .add(Feature::SchemaMigration)
      .add(Feature::ProtocolNegotiation)
      .add(Feature::FencedStaleRejection)
      .add(Feature::EpochAttestation);
  spec.host = "127.0.0.1";
  spec.port = 7001;
  return spec;
}

}  // namespace

int main() {
  using namespace fabric::evolution;

  const ComponentSpec source = spec_of("node-a", "1.0.0", "1.0", 1);
  ComponentSpec target = spec_of("node-b", "2.0.0", "1.1", 2);
  target.port = 7002;

  // 1. A decision produced by the Fabric Compatibility Registry.
  CompatibilityDecision decision;
  decision.decision_id = "compat-1.0.0-2.0.0";
  decision.registry = "fabric-compatibility-registry";
  decision.subject = source.id;
  decision.source = source.software;
  decision.target = target.software;
  decision.source_protocol = source.protocol;
  decision.target_protocol = target.protocol;
  decision.source_schema = source.schema;
  decision.target_schema = target.schema;
  decision.verdict = CompatibilityVerdict::SupportedWithConstraints;
  decision.rollback_permitted = true;
  decision.max_mixed_version_operations = 10000;
  decision.max_mixed_version_ms = 600000;
  decision.certified_features = source.features.intersect(target.features);
  decision.constraints.push_back(CompatibilityConstraint{"mixed_version_window", "bounded"});
  decision.decided_at = EpochNumber::from_value(1);
  if (const Status sealed = decision.seal(); !sealed.ok()) {
    std::fprintf(stderr, "cannot seal the decision: %s\n", sealed.to_string().c_str());
    return 1;
  }
  std::printf("compatibility evidence digest: %s\n", decision.evidence_digest.to_short_hex().c_str());

  // 2. The manifest that declares the evolution.
  EvolutionManifest manifest;
  manifest.id = *ManifestId::parse("manifest-example");
  manifest.campaign = *CampaignId::parse("campaign-example");
  manifest.shard = *ShardId::parse("shard-0");
  manifest.source = source;
  manifest.target = target;
  manifest.compatibility = decision;
  manifest.window.max_operations = 10000;
  manifest.window.max_duration_ms = 600000;
  manifest.window.min_participants = 2;
  manifest.protocol.accepted = {source.protocol, target.protocol};
  manifest.protocol.required_features.add(Feature::SnapshotTransfer)
      .add(Feature::IncrementalCatchUp)
      .add(Feature::FencedStaleRejection);
  manifest.protocol.allowed_features = decision.certified_features;
  manifest.rollback.kind = RollbackKind::RollbackIfNoBoundaryCrossed;
  manifest.rollback.recovery_plan =
      "restore the predecessor and retire the successor before any boundary is crossed";

  MigrationStepSpec rename;
  rename.id = *MigrationStepId::parse("step-rename-region");
  rename.from = SchemaVersion::from_value(1);
  rename.to = SchemaVersion::from_value(2);
  rename.function = "rename_field";
  rename.parameters.set("from", Json("region"));
  rename.parameters.set("to", Json("zone"));
  rename.parameters.set("optional", Json("true"));
  rename.deterministic = true;
  rename.reversible = true;
  manifest.migrations.push_back(rename);

  if (const Status sealed = manifest.seal(); !sealed.ok()) {
    std::fprintf(stderr, "manifest admission refused: %s\n", sealed.to_string().c_str());
    return 1;
  }
  std::printf("manifest %s sealed, digest %s\n", manifest.id.str().c_str(),
              manifest.digest.to_short_hex().c_str());
  std::printf("mixed-version window: %llu operations / %llu ms\n",
              static_cast<unsigned long long>(manifest.effective_window().max_operations),
              static_cast<unsigned long long>(manifest.effective_window().max_duration_ms));

  // 3. State migration across the declared chain, and back again.
  StateDocument state(SchemaVersion::from_value(1));
  if (const Status put = state.put("region", "eu-west"); !put.ok()) {
    return 1;
  }
  (void)state.put("replicas", "3");
  const Digest before = state.digest();

  StateMigrator migrator;
  StateDocument working = state;
  auto forward = migrator.migrate_forward(working, manifest.migrations);
  if (!forward.ok()) {
    std::fprintf(stderr, "migration refused: %s\n", forward.status().to_string().c_str());
    return 1;
  }
  std::printf("migrated schema %u -> %u, zone=%s\n", forward.value().from.value(),
              forward.value().to.value(), working.get("zone").value_or("").c_str());

  auto backward = migrator.migrate_backward(working, manifest.migrations);
  if (!backward.ok()) {
    std::fprintf(stderr, "downgrade refused: %s\n", backward.status().to_string().c_str());
    return 1;
  }
  std::printf("downgrade restored the original digest: %s\n",
              working.digest() == before ? "yes" : "no");

  // 4. Declaring an irreversible boundary changes the recovery strategy, and the
  //    runtime then refuses to downgrade at all.
  MigrationStepSpec drop;
  drop.id = *MigrationStepId::parse("step-drop-legacy");
  drop.from = SchemaVersion::from_value(2);
  drop.to = SchemaVersion::from_value(3);
  drop.function = "remove_field";
  drop.parameters.set("key", Json("replicas"));
  drop.deterministic = true;
  drop.reversible = false;
  drop.irreversible_boundary = true;

  StateDocument crossing(SchemaVersion::from_value(2));
  (void)crossing.put("zone", "eu-west");
  (void)crossing.put("replicas", "3");
  auto crossed = migrator.migrate_forward(crossing, {drop});
  if (!crossed.ok()) {
    std::fprintf(stderr, "crossing refused: %s\n", crossed.status().to_string().c_str());
    return 1;
  }
  auto refused = migrator.migrate_backward(crossing, {drop});
  std::printf("downgrade across an irreversible boundary: %s\n",
              refused.ok() ? "ACCEPTED (unexpected)" : refused.status().to_string().c_str());

  // 5. The manifest that declares that boundary must also declare forward
  //    recovery, and the runtime enforces it at admission.
  EvolutionManifest irreversible = manifest;
  irreversible.migrations.push_back(drop);
  irreversible.target.schema = SchemaVersion::from_value(3);
  irreversible.compatibility.target_schema = SchemaVersion::from_value(3);
  (void)irreversible.compatibility.seal();
  const Status missing_plan = irreversible.seal();
  std::printf("manifest with an undeclared boundary: %s\n",
              missing_plan.ok() ? "ADMITTED (unexpected)" : missing_plan.to_string().c_str());
  return 0;
}
