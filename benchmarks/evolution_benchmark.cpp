// Fabric Evolution — benchmarks.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every measurement below counts *completed* work: completed round trips,
// completed durable writes, completed state migrations and completed control-plane
// evolutions. Nothing here measures enqueue or submission latency.
//
// The distributed numbers are produced with the shipped node and controller
// binaries as independent processes over real TCP sockets.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "fabric/evolution/authority.hpp"
#include "fabric/evolution/controller.hpp"
#include "fabric/evolution/manifest.hpp"
#include "fabric/evolution/migration.hpp"
#include "support/cluster.hpp"

namespace {

using namespace fabric::evolution;
using Clock = std::chrono::steady_clock;

struct Measurement {
  std::string name;
  double units = 0;
  double seconds = 0;
  std::string note;

  [[nodiscard]] double per_second() const { return seconds > 0 ? units / seconds : 0; }
};

[[nodiscard]] double elapsed_seconds(Clock::time_point start) {
  return std::chrono::duration<double>(Clock::now() - start).count();
}

Measurement benchmark_frame_round_trips(test::NodeProcess& node, int iterations) {
  Measurement measurement;
  measurement.name = "node.frame_round_trips";
  measurement.note = "request/response round trips over a real TCP socket";
  ServerOptions options;
  const auto start = Clock::now();
  for (int index = 0; index < iterations; ++index) {
    RpcRequest request;
    request.id = "bench";
    request.op = "node.hello";
    auto response = rpc_call("127.0.0.1", node.port, request, options);
    if (!response.ok()) {
      std::fprintf(stderr, "round trip failed: %s\n", response.status().to_string().c_str());
      std::exit(1);
    }
  }
  measurement.units = iterations;
  measurement.seconds = elapsed_seconds(start);
  return measurement;
}

Measurement benchmark_durable_writes(test::NodeProcess& node, const AuthorityClaim& claim,
                                     int iterations) {
  Measurement measurement;
  measurement.name = "node.durable_writes";
  measurement.note = "authority-checked writes committed to durable state";
  const auto start = Clock::now();
  for (int index = 0; index < iterations; ++index) {
    Json body = Json::object();
    body.set("claim", to_json(claim));
    body.set("key", Json("bench-key"));
    body.set("value", Json(std::to_string(index)));
    auto written = test::node_call(node.port, "node.write", body);
    if (!written.ok()) {
      std::fprintf(stderr, "write failed: %s\n", written.status().to_string().c_str());
      std::exit(1);
    }
  }
  measurement.units = iterations;
  measurement.seconds = elapsed_seconds(start);
  return measurement;
}

Measurement benchmark_migrations(int iterations) {
  Measurement measurement;
  measurement.name = "migration.deterministic_steps";
  measurement.note = "complete forward migration chains applied to a versioned document";
  MigrationStepSpec rename;
  rename.id = *MigrationStepId::parse("rename");
  rename.from = SchemaVersion::from_value(1);
  rename.to = SchemaVersion::from_value(2);
  rename.function = "rename_field";
  rename.parameters.set("from", Json("region"));
  rename.parameters.set("to", Json("zone"));
  const auto start = Clock::now();
  for (int index = 0; index < iterations; ++index) {
    StateDocument document(SchemaVersion::from_value(1));
    (void)document.put("region", "eu-west");
    (void)document.put("replicas", "3");
    StateMigrator migrator;
    auto outcome = migrator.migrate_forward(document, {rename});
    if (!outcome.ok()) {
      std::fprintf(stderr, "migration failed\n");
      std::exit(1);
    }
  }
  measurement.units = iterations;
  measurement.seconds = elapsed_seconds(start);
  return measurement;
}

Measurement benchmark_manifest_admission(int iterations) {
  Measurement measurement;
  measurement.name = "manifest.admissions";
  measurement.note = "manifests validated, sealed and digest-verified";
  const auto start = Clock::now();
  for (int index = 0; index < iterations; ++index) {
    const ComponentSpec source =
        test::make_spec("node-a", "1.0.0", "1.0", 1, 41000, test::standard_features());
    const ComponentSpec target =
        test::make_spec("node-b", "2.0.0", "1.1", 2, 41001, test::standard_features());
    const CompatibilityDecision decision = test::make_decision(source, target);
    EvolutionManifest manifest = test::make_manifest(source, target, decision,
                                                     "bench-" + std::to_string(index));
    if (!manifest.verify_digest().ok()) {
      std::fprintf(stderr, "manifest digest verification failed\n");
      std::exit(1);
    }
  }
  measurement.units = iterations;
  measurement.seconds = elapsed_seconds(start);
  return measurement;
}

Measurement benchmark_handoffs(int rounds) {
  Measurement measurement;
  measurement.name = "evolution.completed_handoffs";
  measurement.note = "complete control-plane evolutions including process startup";
  const auto start = Clock::now();
  for (int round = 0; round < rounds; ++round) {
    test::EvolutionFixture fixture;
    fixture.plan();
    fixture.preflight();
    fixture.run();
    if (fixture.phase() != "predecessor_retired") {
      std::fprintf(stderr, "handoff round %d did not complete\n", round);
      std::exit(1);
    }
  }
  measurement.units = rounds;
  measurement.seconds = elapsed_seconds(start);
  return measurement;
}

}  // namespace

int main(int argc, char** argv) {
  int iterations = 200;
  int handoff_rounds = 3;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--iterations" && index + 1 < argc) {
      iterations = std::atoi(argv[++index]);
    } else if (argument == "--handoffs" && index + 1 < argc) {
      handoff_rounds = std::atoi(argv[++index]);
    } else if (argument == "--help") {
      std::printf("usage: %s [--iterations N] [--handoffs N]\n", argv[0]);
      return 0;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", argument.c_str());
      return 2;
    }
  }
  if (iterations <= 0 || handoff_rounds <= 0) {
    std::fprintf(stderr, "iteration counts must be positive\n");
    return 2;
  }

  std::vector<Measurement> measurements;
  {
    test::TempDir directory("bench");
    test::NodeLaunchOptions options;
    options.state_directory = directory.sub("node");
    options.component = "node-a";
    options.features = "epoch_attestation,fenced_stale_rejection,snapshot_transfer";
    test::NodeProcess node = test::launch_node(options);

    measurements.push_back(benchmark_frame_round_trips(node, iterations));

    SeededTokenSource tokens(3);
    const AuthorityToken token = tokens.next_token();
    const auto incarnation = IncarnationId::parse(node.incarnation);
    Json grant = Json::object();
    grant.set("handoff", Json(1));
    grant.set("epoch", Json(1));
    grant.set("generation", Json(1));
    grant.set("token", Json(token.to_compact_string()));
    grant.set("mode", Json("mutating"));
    grant.set("ttl_ms", Json(600000));
    test::must_node(node.port, "node.grant_authority", std::move(grant));
    AuthorityClaim claim;
    claim.shard = *ShardId::parse("shard-0");
    claim.incarnation = *incarnation;
    claim.epoch = EpochNumber::from_value(1);
    claim.generation = Generation::from_value(1);
    claim.token = token;

    // Durable writes are the expensive path by design: each one is committed
    // before it is acknowledged, so the cycle time is dominated by fsync.
    measurements.push_back(
        benchmark_durable_writes(node, claim, (std::max)(20, iterations / 4)));
  }
  measurements.push_back(benchmark_migrations(iterations));
  measurements.push_back(benchmark_manifest_admission((std::max)(20, iterations / 4)));
  measurements.push_back(benchmark_handoffs(handoff_rounds));

  std::printf("%-34s %12s %10s  %s\n", "benchmark", "completed", "seconds", "per second");
  for (const Measurement& measurement : measurements) {
    std::printf("%-34s %12.0f %10.4f  %12.1f   %s\n", measurement.name.c_str(), measurement.units,
                measurement.seconds, measurement.per_second(), measurement.note.c_str());
  }
  return 0;
}
