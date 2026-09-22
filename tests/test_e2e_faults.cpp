// Fabric Evolution — adversarial end-to-end failure injection.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every scenario here runs the shipped binaries as independent processes. The
// faults are injected through the runtime's own bounded fault surface or by
// killing processes, so what is proven is the behaviour of the real system.

#include <string>
#include <thread>
#include <vector>

#include "support/cluster.hpp"
#include "support/test_harness.hpp"

namespace fabric::evolution {

FABRIC_TEST(e2e_faults, corrupted_snapshot_blocks_the_handoff) {
  test::EvolutionFixture fixture;
  fixture.plan();
  fixture.preflight();
  // The predecessor will emit one snapshot whose declared digest does not match
  // its contents.
  test::must_node(fixture.source_port(), "node.faults",
                  Json::object({{"op", Json("node.snapshot.corrupt")},
                                {"action", Json("corrupt")},
                                {"count", Json(1)}}));
  fixture.advance();
  FABRIC_CHECK_EQ(fixture.phase(), std::string("prepared"));
  auto advanced = test::admin_call(fixture.controller_port(), "advance");
  FABRIC_CHECK(!advanced.ok());
  FABRIC_CHECK_EQ(test::status_of(advanced).code(), ErrorCode::IntegrityFailure);
  FABRIC_CHECK_EQ(fixture.phase(), std::string("prepared"));

  // The successor is not authoritative after the failed transfer.
  const Json status = fixture.status();
  FABRIC_CHECK_EQ(json_bool_or(*status.find("target_report"), "has_authority", true), false);
  FABRIC_CHECK_EQ(json_u64_or(status.find("authority")->at(0), "mutating_authority_count", 0),
                  static_cast<std::uint64_t>(1));

  // A clean retry succeeds: the fault was consumed exactly once.
  fixture.advance();
  FABRIC_CHECK_EQ(fixture.phase(), std::string("snapshot_synchronized"));
  fixture.run();
  FABRIC_CHECK_EQ(fixture.phase(), std::string("predecessor_retired"));
}

FABRIC_TEST(e2e_faults, lost_fence_acknowledgement_defers_without_duplicating_authority) {
  test::EvolutionFixture fixture;
  fixture.plan();
  fixture.preflight();
  fixture.advance_until("readiness_verified");
  FABRIC_CHECK_EQ(fixture.phase(), std::string("readiness_verified"));

  // The predecessor applies the fence but the reply is lost.
  test::must_node(fixture.source_port(), "node.faults",
                  Json::object({{"op", Json("node.fence")},
                                {"action", Json("drop_response")},
                                {"count", Json(1)}}));
  auto deferred = test::admin_call(fixture.controller_port(), "advance");
  FABRIC_CHECK(!deferred.ok());
  FABRIC_CHECK_EQ(test::status_of(deferred).code(), ErrorCode::NotReady);
  FABRIC_CHECK_EQ(fixture.phase(), std::string("readiness_verified"));

  // No authority was granted while the fence was outstanding: the shard has no
  // mutating owner rather than two.
  const Json status = fixture.status();
  FABRIC_CHECK_EQ(json_u64_or(status.find("authority")->at(0), "mutating_authority_count", 0),
                  static_cast<std::uint64_t>(0));
  FABRIC_CHECK_EQ(json_bool_or(*status.find("target_report"), "has_authority", true), false);

  // Retrying the same fence is idempotent on the node and completes the phase.
  fixture.advance();
  FABRIC_CHECK_EQ(fixture.phase(), std::string("predecessor_fenced"));
  fixture.run();
  FABRIC_CHECK_EQ(fixture.phase(), std::string("predecessor_retired"));
  const Json final_status = fixture.status();
  FABRIC_CHECK_EQ(json_u64_or(final_status.find("authority")->at(0), "mutating_authority_count", 0),
                  static_cast<std::uint64_t>(1));
}

FABRIC_TEST(e2e_faults, protocol_major_mismatch_cannot_even_be_declared) {
  // A manifest that would require two participants with incompatible protocol
  // majors is rejected at admission, so no campaign can ever start from it.
  const ComponentSpec source = test::make_spec("node-a", "1.0.0", "1.0", 1, 41000,
                                               test::standard_features());
  const ComponentSpec target = test::make_spec("node-b", "2.0.0", "2.0", 1, 41001,
                                               test::standard_features());
  CompatibilityDecision decision = test::make_decision(source, target);
  EvolutionManifest manifest;
  manifest.id = *ManifestId::parse("manifest-major-mismatch");
  manifest.campaign = *CampaignId::parse("campaign-major-mismatch");
  manifest.shard = *ShardId::parse("shard-0");
  manifest.source = source;
  manifest.target = target;
  manifest.compatibility = decision;
  manifest.window.max_operations = 10;
  manifest.window.max_duration_ms = 1000;
  manifest.protocol.accepted = {ProtocolVersion::of(1, 0), ProtocolVersion::of(2, 0)};
  manifest.rollback.kind = RollbackKind::RollbackIfNoBoundaryCrossed;
  manifest.rollback.recovery_plan = "restore the predecessor";
  FABRIC_CHECK_ERR(manifest.validate(), ErrorCode::UnsupportedProtocol);
  FABRIC_CHECK_ERR(manifest.seal(), ErrorCode::UnsupportedProtocol);
}

FABRIC_TEST(e2e_faults, feature_missing_from_the_successor_is_refused) {
  test::FixtureOptions options;
  options.target_supports_catch_up = false;
  test::EvolutionFixture fixture(options);
  fixture.plan();
  auto preflight = test::admin_call(fixture.controller_port(), "preflight");
  FABRIC_CHECK(!preflight.ok());
  FABRIC_CHECK_EQ(test::status_of(preflight).code(), ErrorCode::FeatureNotNegotiated);
}

FABRIC_TEST(e2e_faults, stale_or_unsupported_evidence_is_refused_at_plan) {
  test::EvolutionFixture fixture;
  // The registry now says something the manifest's embedded evidence does not.
  const ComponentSpec source_spec = test::make_spec("node-a", "1.0.0", "1.0", 1, fixture.source_port(),
                                                    test::standard_features());
  const ComponentSpec target_spec = test::make_spec("node-b", "2.0.0", "1.1", 2, fixture.target_port(),
                                                    test::standard_features());
  CompatibilityDecision unsupported = test::make_decision(source_spec, target_spec, false, true);
  test::write_compatibility_registry(fixture.root() + "/compatibility.json", {unsupported});
  fixture.restart_controller();
  auto refused = test::admin_call(fixture.controller_port(), "plan",
                                  Json::object({{"manifest", to_json(fixture.manifest())}}));
  FABRIC_CHECK(!refused.ok());
  FABRIC_CHECK_EQ(test::status_of(refused).code(), ErrorCode::CompatibilityInsufficient);

  // With the refresh policy a *verdict-preserving* refresh is accepted, while a
  // verdict change is still refused.
  test::EvolutionFixture tolerant(test::FixtureOptions{.lease_ttl_ms = 8000});
  CompatibilityDecision renamed = tolerant.manifest().compatibility;
  renamed.decision_id = "compat-refreshed";
  FABRIC_CHECK_OK(renamed.seal());
  test::write_compatibility_registry(tolerant.root() + "/compatibility.json", {renamed});
  tolerant.restart_controller();
  auto strict = test::admin_call(tolerant.controller_port(), "plan",
                                 Json::object({{"manifest", to_json(tolerant.manifest())}}));
  FABRIC_CHECK(!strict.ok());
  FABRIC_CHECK_EQ(test::status_of(strict).code(), ErrorCode::CompatibilityInsufficient);

  // A registry without any decision for the pair is refused as insufficient too.
  test::write_compatibility_registry(tolerant.root() + "/compatibility.json", {});
  tolerant.restart_controller();
  auto missing = test::admin_call(tolerant.controller_port(), "plan",
                                  Json::object({{"manifest", to_json(tolerant.manifest())}}));
  FABRIC_CHECK(!missing.ok());
  FABRIC_CHECK_EQ(test::status_of(missing).code(), ErrorCode::CompatibilityInsufficient);
}

FABRIC_TEST(e2e_faults, migration_failure_stops_the_handoff_before_authority_moves) {
  test::EvolutionFixture fixture;
  // Point the rename at a field the predecessor never wrote.
  EvolutionManifest manifest = fixture.manifest();
  // Strict step: the manifest promises the field exists, so a state that lacks it
  // must fail the migration instead of quietly doing nothing.
  manifest.migrations.front().parameters.erase("optional");
  manifest.migrations.front().parameters.set("from", Json("absent_field"));
  FABRIC_CHECK_OK(manifest.seal());
  test::must_admin(fixture.controller_port(), "plan",
                   Json::object({{"manifest", to_json(manifest)}}));
  fixture.preflight();
  fixture.advance_until("snapshot_synchronized");
  FABRIC_CHECK_EQ(fixture.phase(), std::string("snapshot_synchronized"));
  auto advanced = test::admin_call(fixture.controller_port(), "advance");
  FABRIC_CHECK(!advanced.ok());
  FABRIC_CHECK_EQ(test::status_of(advanced).code(), ErrorCode::MigrationFailure);
  FABRIC_CHECK_EQ(fixture.phase(), std::string("snapshot_synchronized"));
  const Json status = fixture.status();
  FABRIC_CHECK_EQ(json_bool_or(*status.find("target_report"), "has_authority", true), false);
}

FABRIC_TEST(e2e_faults, stale_replicated_records_are_refused) {
  test::EvolutionFixture fixture;
  fixture.plan();
  fixture.preflight();
  fixture.advance_until("prepared");

  // A successor accepts only records bound to the generation it was prepared for.
  Json binding = Json::object();
  binding.set("incarnation", to_json(fixture.claim_of(fixture.source()).incarnation));
  binding.set("epoch", Json(1));
  binding.set("generation", Json(99));
  binding.set("token", Json(fixture.claim_of(fixture.source()).token.to_compact_string()));
  binding.set("schema", Json(1));
  binding.set("handoff", Json(1));
  Json body = Json::object();
  body.set("handoff", Json(1));
  body.set("source", std::move(binding));
  body.set("records", Json::array());
  FABRIC_CHECK_ERR(test::node_call(fixture.target_port(), "node.apply_records", body),
                   ErrorCode::StaleGeneration);
}

FABRIC_TEST(e2e_faults, abort_before_authority_moves_rolls_back) {
  test::EvolutionFixture fixture;
  fixture.plan();
  fixture.preflight();
  fixture.advance_until("caught_up");
  const AuthorityClaim predecessor_claim = fixture.claim_of(fixture.source());

  test::must_admin(fixture.controller_port(), "abort");
  FABRIC_CHECK_EQ(fixture.campaign_state(), std::string("aborted"));
  FABRIC_CHECK_EQ(fixture.phase(), std::string("aborted"));

  // The predecessor still owns the shard and can still serve writes.
  const Json status = fixture.status();
  FABRIC_CHECK_EQ(json_u64_or(status.find("authority")->at(0), "mutating_authority_count", 0),
                  static_cast<std::uint64_t>(1));
  const Json written = fixture.write_to(fixture.source(), "region", "eu-central");
  FABRIC_CHECK(json_u64_or(written, "lsn", 0) > 0);
  FABRIC_CHECK_NE(written.dump(), std::string());
  FABRIC_CHECK_EQ(predecessor_claim.shard.str(), std::string("shard-0"));
}

FABRIC_TEST(e2e_faults, abort_after_an_irreversible_boundary_goes_forward) {
  test::FixtureOptions options;
  options.irreversible = true;
  test::EvolutionFixture fixture(options);
  fixture.plan();
  fixture.preflight();
  fixture.advance_until("caught_up");
  const Json status = fixture.status();
  FABRIC_CHECK_EQ(json_bool_or(*status.find("campaign"), "irreversible_boundary_crossed", false), true);

  test::must_admin(fixture.controller_port(), "abort");
  FABRIC_CHECK_EQ(fixture.campaign_state(), std::string("completed"));
  FABRIC_CHECK_EQ(fixture.phase(), std::string("predecessor_retired"));
  const Json final_status = fixture.status();
  FABRIC_CHECK_EQ(json_u64_or(final_status.find("authority")->at(0), "mutating_authority_count", 0),
                  static_cast<std::uint64_t>(1));
}

FABRIC_TEST(e2e_faults, pause_inside_the_authority_window_is_deferred) {
  test::EvolutionFixture fixture;
  fixture.plan();
  fixture.preflight();
  fixture.advance_until("predecessor_fenced");
  auto paused = test::admin_call(fixture.controller_port(), "pause");
  FABRIC_CHECK(!paused.ok());
  FABRIC_CHECK_EQ(test::status_of(paused).code(), ErrorCode::NotReady);
  FABRIC_CHECK(test::status_of(paused).message().find("authority is mid-transfer") !=
                std::string::npos);
  FABRIC_CHECK_EQ(fixture.campaign_state(), std::string("running"));

  // At a later phase boundary, after the authority window has closed, the pause
  // is honoured rather than deferred.
  fixture.advance();
  FABRIC_CHECK_EQ(fixture.phase(), std::string("authority_transferred"));
  auto still_deferred = test::admin_call(fixture.controller_port(), "pause");
  FABRIC_CHECK(!still_deferred.ok());
  FABRIC_CHECK_EQ(test::status_of(still_deferred).code(), ErrorCode::NotReady);
  fixture.advance();
  FABRIC_CHECK_EQ(fixture.phase(), std::string("successor_verified"));
  test::must_admin(fixture.controller_port(), "pause");
  FABRIC_CHECK_EQ(fixture.campaign_state(), std::string("paused"));
  auto advanced = test::admin_call(fixture.controller_port(), "advance");
  FABRIC_CHECK(!advanced.ok());
  FABRIC_CHECK_EQ(test::status_of(advanced).code(), ErrorCode::NotReady);
  test::must_admin(fixture.controller_port(), "resume");
  FABRIC_CHECK_EQ(fixture.phase(), std::string("predecessor_retired"));
}

FABRIC_TEST(e2e_faults, self_fencing_after_lease_expiry_stops_the_predecessor) {
  test::FixtureOptions options;
  options.lease_ttl_ms = 1200;
  test::EvolutionFixture fixture(options);
  fixture.plan();
  fixture.preflight();
  fixture.advance_until("snapshot_synchronized");

  // The incumbent holds a valid lease right now.
  FABRIC_CHECK(fixture.try_write_to(fixture.source(), "region", "eu-west").ok());

  // Wait for the lease to lapse without the controller renewing it: authority is
  // lost by expiry, so the node refuses to mutate on its own.
  for (int attempt = 0; attempt < 200; ++attempt) {
    const Json report = test::must_node(fixture.source_port(), "node.status");
    if (!json_bool_or(*report.find("report"), "has_authority", true)) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  auto expired = fixture.try_write_to(fixture.source(), "region", "eu-north");
  FABRIC_CHECK(!expired.ok());
  FABRIC_CHECK_EQ(test::status_of(expired).code(), ErrorCode::StaleAuthority);

  // Reads continue, reported as non-authoritative rather than silently trusted.
  const Json read = fixture.read_from(fixture.source(), "region");
  FABRIC_CHECK_EQ(json_bool_or(read, "authoritative", false), false);
}

FABRIC_TEST(e2e_faults, unknown_admin_operations_are_refused) {
  test::EvolutionFixture fixture;
  FABRIC_CHECK_ERR(test::admin_call(fixture.controller_port(), "nonexistent"),
                   ErrorCode::NotFound);
  FABRIC_CHECK_ERR(test::admin_call(fixture.controller_port(), "preflight"), ErrorCode::NotFound);
}

}  // namespace fabric::evolution
