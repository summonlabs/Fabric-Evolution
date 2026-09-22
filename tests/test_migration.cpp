// Fabric Evolution — state migration tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <string>

#include "fabric/evolution/migration.hpp"
#include "support/test_harness.hpp"

namespace fabric::evolution {
namespace {

MigrationStepSpec step_of(const char* id, std::uint32_t from, std::uint32_t to, const char* function) {
  MigrationStepSpec spec;
  spec.id = *MigrationStepId::parse(id);
  spec.from = SchemaVersion::from_value(from);
  spec.to = SchemaVersion::from_value(to);
  spec.function = function;
  spec.deterministic = true;
  spec.reversible = true;
  return spec;
}

StateDocument document_with_region() {
  StateDocument document(SchemaVersion::from_value(1));
  FABRIC_CHECK_OK(document.put("region", "eu-west"));
  FABRIC_CHECK_OK(document.put("replicas", "3"));
  return document;
}

}  // namespace

FABRIC_TEST(migration, forward_and_reverse_are_exact_inverses) {
  MigrationStepSpec rename = step_of("rename", 1, 2, "rename_field");
  rename.parameters.set("from", Json("region"));
  rename.parameters.set("to", Json("zone"));
  StateDocument original = document_with_region();
  const Digest before = original.digest();

  StateMigrator migrator;
  StateDocument working = original;
  auto forward = migrator.migrate_forward(working, {rename});
  FABRIC_CHECK_OK(forward);
  FABRIC_CHECK_EQ(working.schema(), SchemaVersion::from_value(2));
  FABRIC_CHECK(working.get("zone").has_value());
  FABRIC_CHECK(!working.get("region").has_value());
  FABRIC_CHECK_EQ(working.get("zone").value(), std::string("eu-west"));

  auto backward = migrator.migrate_backward(working, {rename});
  FABRIC_CHECK_OK(backward);
  FABRIC_CHECK_EQ(working.digest(), before);
  FABRIC_CHECK_EQ(working.get("region").value(), std::string("eu-west"));
}

FABRIC_TEST(migration, downgrade_across_an_irreversible_boundary_is_refused) {
  MigrationStepSpec drop = step_of("drop", 1, 2, "remove_field");
  drop.parameters.set("key", Json("region"));
  drop.reversible = false;
  drop.irreversible_boundary = true;
  StateMigrator migrator;
  StateDocument document = document_with_region();
  auto forward = migrator.migrate_forward(document, {drop});
  FABRIC_CHECK_OK(forward);
  FABRIC_CHECK(forward.value().crossed_irreversible_boundary);
  FABRIC_CHECK(!document.get("region").has_value());
  FABRIC_CHECK_ERR(migrator.migrate_backward(document, {drop}), ErrorCode::IrreversibleBoundary);
}

FABRIC_TEST(migration, evidence_digest_binds_state_to_the_source_generation) {
  MigrationStepSpec rename = step_of("rename", 1, 2, "rename_field");
  rename.parameters.set("from", Json("region"));
  rename.parameters.set("to", Json("zone"));
  const StateDocument document = document_with_region();
  rename.evidence_digest = migration_evidence_digest(rename, document.digest());

  StateMigrator migrator;
  StateDocument bound = document;
  FABRIC_CHECK_OK(migrator.migrate_forward(bound, {rename}));

  // The same step applied to different state no longer matches its evidence.
  StateDocument different = document_with_region();
  FABRIC_CHECK_OK(different.put("replicas", "5"));
  FABRIC_CHECK_ERR(migrator.migrate_forward(different, {rename}), ErrorCode::IntegrityFailure);
}

FABRIC_TEST(migration, missing_fields_fail_rather_than_silently_succeeding) {
  MigrationStepSpec rename = step_of("rename", 1, 2, "rename_field");
  rename.parameters.set("from", Json("absent"));
  rename.parameters.set("to", Json("zone"));
  StateDocument document = document_with_region();
  StateMigrator migrator;
  FABRIC_CHECK_ERR(migrator.migrate_forward(document, {rename}), ErrorCode::MigrationFailure);
  FABRIC_CHECK_EQ(document.schema(), SchemaVersion::from_value(1));

  MigrationStepSpec remove = step_of("remove", 1, 2, "remove_field");
  remove.parameters.set("key", Json("absent"));
  StateDocument other = document_with_region();
  FABRIC_CHECK_ERR(migrator.migrate_forward(other, {remove}), ErrorCode::MigrationFailure);
}

FABRIC_TEST(migration, rename_refuses_to_overwrite_an_existing_field) {
  StateDocument document(SchemaVersion::from_value(1));
  FABRIC_CHECK_OK(document.put("region", "a"));
  FABRIC_CHECK_OK(document.put("zone", "b"));
  MigrationStepSpec rename = step_of("rename", 1, 2, "rename_field");
  rename.parameters.set("from", Json("region"));
  rename.parameters.set("to", Json("zone"));
  StateMigrator migrator;
  FABRIC_CHECK_ERR(migrator.migrate_forward(document, {rename}), ErrorCode::MigrationFailure);
}

FABRIC_TEST(migration, unknown_functions_and_parameters_are_refused) {
  MigrationStepSpec unknown = step_of("unknown", 1, 2, "run_arbitrary_code");
  auto resolved = resolve_migration_step(unknown);
  FABRIC_CHECK_ERR(resolved, ErrorCode::Unsupported);

  MigrationStepSpec missing_argument = step_of("wrap", 1, 2, "wrap_value");
  missing_argument.parameters.set("key", Json("region"));
  StateDocument document = document_with_region();
  StateMigrator migrator;
  FABRIC_CHECK_ERR(migrator.migrate_forward(document, {missing_argument}), ErrorCode::Unsupported);

  MigrationStepSpec non_deterministic = step_of("random", 1, 2, "identity");
  non_deterministic.deterministic = false;
  auto rejected = resolve_migration_step(non_deterministic);
  FABRIC_CHECK_ERR(rejected, ErrorCode::Unsupported);
}

FABRIC_TEST(migration, wrap_and_unwrap_round_trip) {
  MigrationStepSpec wrap = step_of("wrap", 1, 2, "wrap_value");
  wrap.parameters.set("key", Json("region"));
  wrap.parameters.set("prefix", Json("z:"));
  wrap.parameters.set("suffix", Json(":v2"));
  StateDocument document = document_with_region();
  const Digest before = document.digest();
  StateMigrator migrator;
  FABRIC_CHECK_OK(migrator.migrate_forward(document, {wrap}));
  FABRIC_CHECK_EQ(document.get("region").value(), std::string("z:eu-west:v2"));
  FABRIC_CHECK_OK(migrator.migrate_backward(document, {wrap}));
  FABRIC_CHECK_EQ(document.digest(), before);
}

FABRIC_TEST(migration, non_contiguous_paths_are_refused) {
  MigrationStepSpec first = step_of("first", 1, 2, "identity");
  MigrationStepSpec gap = step_of("gap", 3, 4, "identity");
  StateDocument document = document_with_region();
  StateMigrator migrator;
  FABRIC_CHECK_ERR(migrator.migrate_forward(document, {first, gap}), ErrorCode::MigrationFailure);
  FABRIC_CHECK_EQ(document.schema(), SchemaVersion::from_value(1));
}

FABRIC_TEST(migration, size_bounds_are_enforced) {
  StateDocument document(SchemaVersion::from_value(1));
  FABRIC_CHECK_ERR(document.put(std::string(kMaxStateKeyBytes + 1, 'k'), "v"), ErrorCode::BoundsExceeded);
  FABRIC_CHECK_ERR(document.put("k", std::string(kMaxStateValueBytes + 1, 'v')), ErrorCode::BoundsExceeded);
  FABRIC_CHECK_ERR(document.put("", "v"), ErrorCode::BoundsExceeded);
  FABRIC_CHECK_ERR(document.remove("absent"), ErrorCode::NotFound);
}

FABRIC_TEST(migration, document_json_round_trip_and_validation) {
  StateDocument document = document_with_region();
  const Json encoded = document.to_json();
  const auto decoded = StateDocument::from_json(encoded);
  FABRIC_CHECK_OK(decoded);
  FABRIC_CHECK_EQ(decoded.value().digest(), document.digest());
  FABRIC_CHECK_ERR(StateDocument::from_json(Json::array()), ErrorCode::Malformed);
  Json broken = encoded;
  broken.set("schema", Json(0));
  FABRIC_CHECK_ERR(StateDocument::from_json(broken), ErrorCode::Malformed);
  Json non_string = encoded;
  non_string.set("fields", Json::object({{"k", Json(1)}}));
  FABRIC_CHECK_ERR(StateDocument::from_json(non_string), ErrorCode::Malformed);
}

FABRIC_TEST(migration, journal_is_bounded_and_detects_boundaries) {
  MigrationJournal journal(4);
  const ShardId shard = *ShardId::parse("shard-0");
  const ComponentId component = *ComponentId::parse("node-a");
  for (std::uint32_t index = 1; index <= 10; ++index) {
    MigrationJournalRecord record;
    record.shard = shard;
    record.component = component;
    record.step = *MigrationStepId::parse("step-" + std::to_string(index));
    record.from = SchemaVersion::from_value(index);
    record.to = SchemaVersion::from_value(index + 1);
    record.irreversible_boundary = index == 3;
    FABRIC_CHECK_OK(journal.record(record));
  }
  FABRIC_CHECK_EQ(journal.records().size(), static_cast<std::size_t>(4));
  FABRIC_CHECK(journal.irreversible_crossed(shard, component) == false);
  FABRIC_CHECK(journal.has_applied(shard, component, *MigrationStepId::parse("step-8"),
                                   SchemaVersion::from_value(8), SchemaVersion::from_value(9)));
  FABRIC_CHECK(!journal.has_applied(shard, component, *MigrationStepId::parse("step-1"),
                                    SchemaVersion::from_value(1), SchemaVersion::from_value(2)));

  MigrationJournalRecord irreversible;
  irreversible.shard = shard;
  irreversible.component = component;
  irreversible.step = *MigrationStepId::parse("step-boundary");
  irreversible.from = SchemaVersion::from_value(20);
  irreversible.to = SchemaVersion::from_value(21);
  irreversible.irreversible_boundary = true;
  FABRIC_CHECK_OK(journal.record(irreversible));
  FABRIC_CHECK(journal.irreversible_crossed(shard, component));
  FABRIC_CHECK_EQ(journal.highest_schema(shard, component).value(), SchemaVersion::from_value(21));

  const Json encoded = journal.to_json();
  MigrationJournal restored(4);
  FABRIC_CHECK_OK(restored.load(encoded));
  FABRIC_CHECK_EQ(restored.records().size(), journal.records().size());
  FABRIC_CHECK_ERR(restored.load(Json::array()), ErrorCode::Malformed);
}

}  // namespace fabric::evolution
