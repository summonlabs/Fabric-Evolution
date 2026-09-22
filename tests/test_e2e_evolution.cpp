// Fabric Evolution — end-to-end evolution over independent processes and sockets.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// This suite is the repository's primary proof surface. Every test in it starts
// real fabric-evolution-node and fabric-evolution-controller processes, talks to
// them over real TCP sockets, and kills and restarts them at handoff phase
// boundaries. Nothing here is simulated in-process.

#include <string>
#include <vector>

#include "support/cluster.hpp"
#include "support/test_harness.hpp"

namespace fabric::evolution {

FABRIC_TEST(e2e, handoff_completes_with_continuous_service_and_migrated_state) {
  test::EvolutionFixture fixture;
  fixture.plan();
  fixture.preflight();

  // Representative client traffic against the incumbent before the handoff.
  for (int index = 0; index < 5; ++index) {
    fixture.write_to(fixture.source(), "region", "eu-west");
    fixture.write_to(fixture.source(), "counter", std::to_string(index));
  }
  const AuthorityClaim predecessor_claim = fixture.claim_of(fixture.source());
  const Json before = fixture.read_from(fixture.source(), "region");
  FABRIC_CHECK_EQ(json_string_or(before, "value", ""), std::string("eu-west"));
  FABRIC_CHECK_EQ(json_bool_or(before, "authoritative", false), true);
  const std::uint64_t lsn_before = json_u64_or(before, "lsn", 0);
  FABRIC_CHECK(lsn_before >= 5);

  fixture.run();
  FABRIC_CHECK_EQ(fixture.campaign_state(), std::string("completed"));
  FABRIC_CHECK_EQ(fixture.phase(), std::string("predecessor_retired"));

  // Authority is unique and has moved to a strictly newer generation.
  const Json status = fixture.status();
  const Json* authority = status.find("authority");
  FABRIC_CHECK(authority != nullptr);
  FABRIC_CHECK_EQ(authority->size(), static_cast<std::size_t>(1));
  FABRIC_CHECK_EQ(json_u64_or(authority->at(0), "mutating_authority_count", 0),
                  static_cast<std::uint64_t>(1));
  const Json& lease = *authority->at(0).find("mutating");
  FABRIC_CHECK(json_u64_or(lease, "generation", 0) > predecessor_claim.generation.value());
  FABRIC_CHECK_NE(json_string_or(lease, "token", ""), predecessor_claim.token.to_compact_string());

  // The successor serves the migrated state: the declared rename step moved the
  // region field to zone, and the value survived the snapshot and the catch-up.
  const Json migrated = fixture.read_from(fixture.target(), "zone");
  FABRIC_CHECK_EQ(json_string_or(migrated, "value", ""), std::string("eu-west"));
  FABRIC_CHECK_EQ(json_u64_or(migrated, "schema", 0), static_cast<std::uint64_t>(2));
  FABRIC_CHECK_EQ(json_bool_or(migrated, "authoritative", false), true);
  const Json old_field = fixture.read_from(fixture.target(), "region");
  FABRIC_CHECK_EQ(json_bool_or(old_field, "found", true), false);
  const Json counter = fixture.read_from(fixture.target(), "counter");
  FABRIC_CHECK_EQ(json_string_or(counter, "value", ""), std::string("4"));

  // The retired predecessor refuses to mutate even with the credentials it held.
  Json stale_write = Json::object();
  stale_write.set("claim", to_json(predecessor_claim));
  stale_write.set("key", Json("region"));
  stale_write.set("value", Json("us-east"));
  FABRIC_CHECK_ERR(test::node_call(fixture.source_port(), "node.write", stale_write),
                   ErrorCode::NotAuthoritative);

  // Continuous service: the successor accepts traffic after the handoff.
  const Json after = fixture.write_to(fixture.target(), "region", "us-east");
  FABRIC_CHECK(json_u64_or(after, "lsn", 0) > lsn_before);
  const Json read_back = fixture.read_from(fixture.target(), "region");
  FABRIC_CHECK_EQ(json_string_or(read_back, "value", ""), std::string("us-east"));
}

FABRIC_TEST(e2e, handoff_phase_progression_is_ordered_and_inspectable) {
  test::EvolutionFixture fixture;
  fixture.plan();
  fixture.preflight();
  const std::vector<std::string> expected = {
      "prepared",           "snapshot_synchronized", "caught_up",
      "readiness_verified", "predecessor_fenced",   "authority_transferred",
      "successor_verified", "predecessor_retired",
  };
  for (const std::string& phase : expected) {
    fixture.advance();
    FABRIC_CHECK_EQ(fixture.phase(), phase);
  }
  const Json handoff = test::must_admin(fixture.controller_port(), "handoff");
  const Json& record = *handoff.find("handoff");
  FABRIC_CHECK_EQ(json_u64_or(record, "attempt", 0), static_cast<std::uint64_t>(1));
  const Json* history = record.find("history");
  FABRIC_CHECK(history != nullptr);
  FABRIC_CHECK_EQ(history->size(), static_cast<std::size_t>(8));
}

FABRIC_TEST(e2e, duplicate_frames_are_idempotent_and_never_skip_a_phase) {
  test::EvolutionFixture fixture;
  fixture.plan();
  fixture.preflight();
  fixture.advance();
  FABRIC_CHECK_EQ(fixture.phase(), std::string("prepared"));

  // Each advance performs exactly one phase; nothing is skipped or repeated.
  fixture.advance();
  FABRIC_CHECK_EQ(fixture.phase(), std::string("snapshot_synchronized"));

  // Replaying the snapshot phase at the node is harmless: the same image installs
  // twice and the successor ends in an identical state.
  const Json snapshot = test::must_node(fixture.source_port(), "node.snapshot",
                                        Json::object({{"handoff", Json(1)}}));
  const std::string first_digest =
      json_string_or(*snapshot.find("snapshot"), "digest", "");
  Json install = Json::object();
  install.set("handoff", Json(1));
  install.set("snapshot", *snapshot.find("snapshot"));
  test::must_node(fixture.target_port(), "node.install_snapshot", install);
  const Json replay = test::must_node(fixture.target_port(), "node.install_snapshot", install);
  FABRIC_CHECK_EQ(json_string_or(replay, "digest", ""), first_digest);
  const Json target_report = test::must_node(fixture.target_port(), "node.status");
  FABRIC_CHECK_EQ(json_u64_or(*target_report.find("report"), "lsn", 0),
                  json_u64_or(*snapshot.find("snapshot"), "lsn", 1));

  fixture.run();
  FABRIC_CHECK_EQ(fixture.phase(), std::string("predecessor_retired"));
}

FABRIC_TEST(e2e, predecessor_kill_and_restart_at_every_phase) {
  const std::vector<std::string> phases = {
      "prepared",           "snapshot_synchronized", "caught_up",
      "readiness_verified", "predecessor_fenced",   "authority_transferred",
      "successor_verified",
  };
  for (const std::string& phase : phases) {
    test::EvolutionFixture fixture;
    fixture.plan();
    fixture.preflight();
    fixture.advance_until(phase);
    FABRIC_CHECK_EQ(fixture.phase(), phase);

    fixture.restart_source();
    test::must_admin(fixture.controller_port(), "reconcile");
    auto completed = test::admin_call(fixture.controller_port(), "run");
    if (!completed.ok()) {
      FABRIC_FAIL("handoff did not complete after a predecessor restart at phase " + phase + ": " +
                  completed.status().to_string());
    }
    FABRIC_CHECK_EQ(fixture.phase(), std::string("predecessor_retired"));
    FABRIC_CHECK_EQ(fixture.campaign_state(), std::string("completed"));

    // Exactly one authoritative owner exists at the end of every run.
    const Json status = fixture.status();
    FABRIC_CHECK_EQ(json_u64_or(status.find("authority")->at(0), "mutating_authority_count", 0),
                    static_cast<std::uint64_t>(1));
  }
}

FABRIC_TEST(e2e, successor_kill_and_restart_at_every_phase) {
  const std::vector<std::string> phases = {
      "prepared",           "snapshot_synchronized", "caught_up",
      "readiness_verified", "predecessor_fenced",   "authority_transferred",
  };
  for (const std::string& phase : phases) {
    test::EvolutionFixture fixture;
    fixture.plan();
    fixture.preflight();
    fixture.advance_until(phase);
    FABRIC_CHECK_EQ(fixture.phase(), phase);

    fixture.restart_target();
    test::must_admin(fixture.controller_port(), "reconcile");
    auto completed = test::admin_call(fixture.controller_port(), "run");
    if (!completed.ok()) {
      FABRIC_FAIL("handoff did not complete after a successor restart at phase " + phase + ": " +
                  completed.status().to_string());
    }
    FABRIC_CHECK_EQ(fixture.phase(), std::string("predecessor_retired"));
  }
}

FABRIC_TEST(e2e, controller_kill_and_restart_at_every_phase) {
  const std::vector<std::string> phases = {
      "prepared",           "snapshot_synchronized", "caught_up",
      "readiness_verified", "predecessor_fenced",   "authority_transferred",
      "successor_verified",
  };
  for (const std::string& phase : phases) {
    test::EvolutionFixture fixture;
    fixture.plan();
    fixture.preflight();
    fixture.advance_until(phase);
    FABRIC_CHECK_EQ(fixture.phase(), phase);

    fixture.restart_controller();
    // The restarted controller must explain what it found before acting.
    const Json reconciled = test::must_admin(fixture.controller_port(), "reconcile");
    FABRIC_CHECK(reconciled.has("handoff"));
    auto completed = test::admin_call(fixture.controller_port(), "run");
    if (!completed.ok()) {
      FABRIC_FAIL("handoff did not complete after a controller restart at phase " + phase + ": " +
                  completed.status().to_string());
    }
    FABRIC_CHECK_EQ(fixture.phase(), std::string("predecessor_retired"));
  }
}

FABRIC_TEST(e2e, killed_predecessor_restarts_the_handoff_for_the_new_incarnation) {
  test::EvolutionFixture fixture;
  fixture.plan();
  fixture.preflight();
  fixture.advance_until("snapshot_synchronized");
  fixture.kill_source();
  fixture.restart_source();
  const Json reconciled = test::must_admin(fixture.controller_port(), "reconcile");
  const Json& record = *reconciled.find("handoff");
  FABRIC_CHECK_EQ(json_string_or(record, "phase", ""), std::string("not_started"));
  FABRIC_CHECK_EQ(json_u64_or(record, "attempt", 0), static_cast<std::uint64_t>(2));
  FABRIC_CHECK_EQ(json_string_or(*record.find("previous_predecessor"), "component", ""),
                  std::string("node-a"));
  fixture.run();
  FABRIC_CHECK_EQ(fixture.phase(), std::string("predecessor_retired"));
}

FABRIC_TEST(e2e, mixed_version_window_bounds_stop_the_campaign) {
  test::EvolutionFixture fixture(test::FixtureOptions{.max_operations = 3});
  fixture.plan();
  fixture.preflight();
  fixture.advance_until("snapshot_synchronized");
  for (int index = 0; index < 8; ++index) {
    fixture.write_to(fixture.source(), "counter", std::to_string(index));
  }
  auto advanced = test::admin_call(fixture.controller_port(), "advance");
  FABRIC_CHECK(!advanced.ok());
  FABRIC_CHECK_EQ(test::status_of(advanced).code(), ErrorCode::NotReady);
  FABRIC_CHECK(test::status_of(advanced).message().find("budget") != std::string::npos);
  FABRIC_CHECK_EQ(fixture.campaign_state(), std::string("paused"));
  FABRIC_CHECK_EQ(fixture.phase(), std::string("snapshot_synchronized"));
}

FABRIC_TEST(e2e, successor_serves_reads_while_the_predecessor_still_writes) {
  test::EvolutionFixture fixture;
  fixture.plan();
  fixture.preflight();
  fixture.advance_until("caught_up");

  const Json status = fixture.status();
  const Json* authority = status.find("authority");
  FABRIC_CHECK_EQ(json_u64_or(authority->at(0), "mutating_authority_count", 0),
                  static_cast<std::uint64_t>(1));
  FABRIC_CHECK_EQ(json_bool_or(authority->at(0), "mutation_slot_free", true), false);

  // The successor answers reads for state it has already replicated even though
  // it does not hold authority yet.
  const Json read = fixture.read_from(fixture.target(), "zone");
  FABRIC_CHECK_EQ(json_bool_or(read, "authoritative", false), false);
  FABRIC_CHECK_EQ(json_u64_or(read, "schema", 0), static_cast<std::uint64_t>(2));
}

FABRIC_TEST(e2e, epoch_journal_records_the_whole_evolution) {
  test::EvolutionFixture fixture;
  fixture.plan();
  fixture.preflight();
  fixture.run();
  const Json epochs = test::must_admin(fixture.controller_port(), "epoch");
  const Json& events = *epochs.find("events");
  bool saw_fence = false;
  bool saw_transfer = false;
  bool saw_campaign = false;
  for (std::size_t index = 0; index < events.size(); ++index) {
    const std::string kind = json_string_or(events.at(index), "kind", "");
    if (kind == "fence_acknowledged") {
      saw_fence = true;
    }
    if (kind == "lease_granted") {
      saw_transfer = true;
    }
    if (kind == "campaign_transition") {
      saw_campaign = true;
    }
  }
  FABRIC_CHECK(saw_fence);
  FABRIC_CHECK(saw_transfer);
  FABRIC_CHECK(saw_campaign);
  FABRIC_CHECK(json_u64_or(epochs, "records", 0) > 0);
  FABRIC_CHECK_EQ(json_bool_or(epochs, "damaged_tail", true), false);
}

FABRIC_TEST(e2e, explanation_is_deterministic_for_the_same_inputs) {
  test::EvolutionFixture fixture;
  fixture.plan();
  fixture.preflight();
  fixture.run();
  const Json first = test::must_admin(fixture.controller_port(), "explain",
                                      Json::object({{"topic", Json("campaign")}}));
  const Json second = test::must_admin(fixture.controller_port(), "explain",
                                       Json::object({{"topic", Json("campaign")}}));
  FABRIC_CHECK_EQ(first.dump(), second.dump());
  const Json window = test::must_admin(fixture.controller_port(), "explain",
                                       Json::object({{"topic", Json("window")}}));
  FABRIC_CHECK_EQ(json_bool_or(window, "open", false), true);
  FABRIC_CHECK_EQ(json_u64_or(window, "operation_limit", 0), static_cast<std::uint64_t>(1000000));
  const Json compatibility = test::must_admin(
      fixture.controller_port(), "explain", Json::object({{"topic", Json("compatibility")}}));
  FABRIC_CHECK_EQ(json_bool_or(compatibility, "satisfied", false), true);
  FABRIC_CHECK_EQ(json_bool_or(compatibility, "refreshed", true), false);
  FABRIC_CHECK_ERR(test::admin_call(fixture.controller_port(), "explain",
                                    Json::object({{"topic", Json("nonsense")}})),
                   ErrorCode::NotFound);
}

}  // namespace fabric::evolution
