// Fabric Evolution — single-node process, durability and incarnation fencing tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <string>
#include <vector>

#include "fabric/evolution/authority.hpp"
#include "support/cluster.hpp"
#include "support/test_harness.hpp"

namespace fabric::evolution {
namespace {

struct NodeFixture {
  test::TempDir directory{"node"};
  test::NodeProcess node;
  SeededTokenSource tokens{7};
  EpochNumber epoch = EpochNumber::from_value(1);
  Generation generation = Generation::from_value(1);
  AuthorityToken token;

  explicit NodeFixture(const std::string& software = "1.0.0", const std::string& protocol = "1.0",
                       std::uint32_t schema = 1, const std::string& component = "node-a",
                       std::uint64_t lease_ttl_ms = 8000) {
    test::NodeLaunchOptions options;
    options.state_directory = directory.sub("state-" + component);
    options.component = component;
    options.software = software;
    options.protocol = protocol;
    options.schema = schema;
    options.features = "snapshot_transfer,incremental_catch_up,schema_migration,protocol_negotiation,"
                       "fenced_stale_rejection,epoch_attestation";
    options.lease_ttl_ms = lease_ttl_ms;
    node = test::launch_node(options);
    token = tokens.next_token();
  }

  void restart() {
    test::NodeLaunchOptions options;
    options.state_directory = node.state_directory;
    options.component = node.component;
    options.software = "1.0.0";
    options.protocol = "1.0";
    FABRIC_CHECK(node.process->terminate());
    node = test::launch_node(options);
  }

  [[nodiscard]] IncarnationId incarnation() const {
    auto parsed = IncarnationId::parse(node.incarnation);
    FABRIC_CHECK(parsed.has_value());
    return *parsed;
  }

  [[nodiscard]] AuthorityClaim claim() const {
    AuthorityClaim claim;
    claim.shard = *ShardId::parse("shard-0");
    claim.incarnation = incarnation();
    claim.epoch = epoch;
    claim.generation = generation;
    claim.token = token;
    return claim;
  }

  void grant(AuthorityMode mode = AuthorityMode::Mutating) {
    Json body = Json::object();
    body.set("handoff", Json(1));
    body.set("epoch", Json(epoch.value()));
    body.set("generation", Json(generation.value()));
    body.set("token", Json(token.to_compact_string()));
    body.set("mode", Json(std::string(mode == AuthorityMode::Mutating ? "mutating" : "read_only_shared")));
    body.set("ttl_ms", Json(8000));
    test::must_node(node.port, "node.grant_authority", std::move(body));
  }
};

}  // namespace

FABRIC_TEST(node_process, hello_and_status_report_identity) {
  NodeFixture fixture;
  const Json hello = test::must_node(fixture.node.port, "node.hello");
  const Json& document = *hello.find("hello");
  FABRIC_CHECK_EQ(json_string_or(document, "component", ""), std::string("node-a"));
  FABRIC_CHECK_EQ(json_string_or(document, "software", ""), std::string("1.0.0"));
  FABRIC_CHECK_EQ(json_string_or(document, "protocol", ""), std::string("1.0"));
  FABRIC_CHECK_EQ(json_u64_or(document, "schema", 0), static_cast<std::uint64_t>(1));
  FABRIC_CHECK_EQ(json_bool_or(document, "holds_authority", true), false);

  const Json status = test::must_node(fixture.node.port, "node.status");
  const Json& report = *status.find("report");
  FABRIC_CHECK_EQ(json_string_or(report, "lifecycle", ""), std::string("serving"));
  FABRIC_CHECK_EQ(json_u64_or(report, "lsn", 99), static_cast<std::uint64_t>(0));
  FABRIC_CHECK_EQ(json_bool_or(report, "has_authority", true), false);
}

FABRIC_TEST(node_process, writes_require_a_matching_live_lease) {
  NodeFixture fixture;
  Json write = Json::object();
  write.set("claim", to_json(fixture.claim()));
  write.set("key", Json("region"));
  write.set("value", Json("eu-west"));
  // No lease has been granted yet.
  FABRIC_CHECK_ERR(test::node_call(fixture.node.port, "node.write", write), ErrorCode::NotAuthoritative);

  fixture.grant();
  const Json applied = test::must_node(fixture.node.port, "node.write", write);
  FABRIC_CHECK_EQ(json_u64_or(applied, "lsn", 0), static_cast<std::uint64_t>(1));

  const Json read = test::must_node(fixture.node.port, "node.read", Json::object({{"key", Json("region")}}));
  FABRIC_CHECK_EQ(json_string_or(read, "value", ""), std::string("eu-west"));
  FABRIC_CHECK_EQ(json_bool_or(read, "authoritative", false), true);
  FABRIC_CHECK_EQ(json_u64_or(read, "lsn", 0), static_cast<std::uint64_t>(1));

  // A claim with a different generation is stale even though the token matches.
  AuthorityClaim stale = fixture.claim();
  stale.generation = Generation::from_value(99);
  Json stale_write = write;
  stale_write.set("claim", to_json(stale));
  FABRIC_CHECK_ERR(test::node_call(fixture.node.port, "node.write", stale_write),
                   ErrorCode::StaleGeneration);

  // A claim that is not even well formed is rejected as malformed, never as a
  // silently defaulted identity.
  AuthorityClaim wrong_token = fixture.claim();
  wrong_token.token = AuthorityToken::nil();
  Json wrong_write = write;
  wrong_write.set("claim", to_json(wrong_token));
  const auto malformed_result = test::node_call(fixture.node.port, "node.write", wrong_write);
  FABRIC_CHECK(!malformed_result.ok());
  FABRIC_CHECK_EQ(test::status_of(malformed_result).code(), ErrorCode::Malformed);
}

FABRIC_TEST(node_process, restart_is_a_new_incarnation_and_old_claims_are_fenced) {
  NodeFixture fixture;
  fixture.grant();
  Json write = Json::object();
  write.set("claim", to_json(fixture.claim()));
  write.set("key", Json("region"));
  write.set("value", Json("eu-west"));
  test::must_node(fixture.node.port, "node.write", write);

  const std::string previous_incarnation = fixture.node.incarnation;
  const std::uint64_t previous_boot = fixture.node.boot;
  const AuthorityClaim previous_claim = fixture.claim();

  fixture.restart();
  FABRIC_CHECK_NE(fixture.node.incarnation, previous_incarnation);
  FABRIC_CHECK(fixture.node.boot > previous_boot);

  // Durable state survives, but authority does not.
  const Json read = test::must_node(fixture.node.port, "node.read",
                                    Json::object({{"key", Json("region")}, {"allow_stale", Json(true)}}));
  FABRIC_CHECK_EQ(json_string_or(read, "value", ""), std::string("eu-west"));
  FABRIC_CHECK_EQ(json_bool_or(read, "authoritative", false), false);
  FABRIC_CHECK_EQ(json_u64_or(read, "lsn", 0), static_cast<std::uint64_t>(1));

  // Even a correctly-addressed claim is refused until the lease is re-granted:
  // deserialised authority is never treated as live authority.
  Json current_write = Json::object();
  current_write.set("claim", to_json(fixture.claim()));
  current_write.set("key", Json("region"));
  current_write.set("value", Json("us-east"));
  FABRIC_CHECK_ERR(test::node_call(fixture.node.port, "node.write", current_write),
                   ErrorCode::NotAuthoritative);

  fixture.grant();
  // With a live lease for the new incarnation, the previous incarnation's
  // credentials are refused as stale rather than being confused with the new one.
  Json stale_write = Json::object();
  stale_write.set("claim", to_json(previous_claim));
  stale_write.set("key", Json("region"));
  stale_write.set("value", Json("us-east"));
  FABRIC_CHECK_ERR(test::node_call(fixture.node.port, "node.write", stale_write),
                   ErrorCode::StaleIncarnation);
  test::must_node(fixture.node.port, "node.write", current_write);
  const Json after = test::must_node(fixture.node.port, "node.read", Json::object({{"key", Json("region")}}));
  FABRIC_CHECK_EQ(json_string_or(after, "value", ""), std::string("us-east"));
  FABRIC_CHECK_EQ(json_u64_or(after, "lsn", 0), static_cast<std::uint64_t>(2));
}

FABRIC_TEST(node_process, fence_is_acknowledged_idempotently_and_stops_mutation) {
  NodeFixture fixture;
  fixture.grant();
  Json fence = Json::object();
  fence.set("handoff", Json(1));
  fence.set("fence_id", Json(11));
  fence.set("target", to_json(fixture.incarnation()));
  const Json acknowledged = test::must_node(fixture.node.port, "node.fence", fence);
  FABRIC_CHECK_EQ(json_bool_or(acknowledged, "acknowledged", false), true);
  FABRIC_CHECK_EQ(json_bool_or(acknowledged, "idempotent", false), false);

  const Json repeated = test::must_node(fixture.node.port, "node.fence", fence);
  FABRIC_CHECK_EQ(json_bool_or(repeated, "idempotent", false), true);

  // A fence aimed at a different incarnation is refused.
  Json wrong_target = fence;
  IncarnationId other = fixture.incarnation();
  other = IncarnationId(other.component(),
                        IncarnationUuid::parse("ffffffffffffffffffffffffffffffff").value(),
                        BootCounter::from_value(9));
  wrong_target.set("target", to_json(other));
  FABRIC_CHECK_ERR(test::node_call(fixture.node.port, "node.fence", wrong_target),
                   ErrorCode::StaleIncarnation);

  Json write = Json::object();
  write.set("claim", to_json(fixture.claim()));
  write.set("key", Json("region"));
  write.set("value", Json("blocked"));
  FABRIC_CHECK_ERR(test::node_call(fixture.node.port, "node.write", write), ErrorCode::StaleAuthority);

  // Re-granting authority to a fenced node is possible, but only explicitly.
  fixture.grant();
  test::must_node(fixture.node.port, "node.write", write);
}

FABRIC_TEST(node_process, retired_node_refuses_everything_mutating) {
  NodeFixture fixture;
  fixture.grant();
  test::must_node(fixture.node.port, "node.retire", Json::object({{"handoff", Json(1)}}));
  Json write = Json::object();
  write.set("claim", to_json(fixture.claim()));
  write.set("key", Json("region"));
  write.set("value", Json("nope"));
  FABRIC_CHECK_ERR(test::node_call(fixture.node.port, "node.write", write), ErrorCode::NotAuthoritative);
  // Retirement is terminal: not even an explicit re-grant revives the node.
  FABRIC_CHECK_ERR(test::node_call(fixture.node.port, "node.grant_authority",
                                   Json::object({{"handoff", Json(1)},
                                                 {"epoch", Json(1)},
                                                 {"generation", Json(1)},
                                                 {"token", Json(fixture.token.to_compact_string())},
                                                 {"mode", Json("mutating")},
                                                 {"ttl_ms", Json(8000)}})),
                   ErrorCode::NotAuthoritative);
  FABRIC_CHECK_ERR(test::node_call(fixture.node.port, "node.write", write), ErrorCode::NotAuthoritative);
  const Json status = test::must_node(fixture.node.port, "node.status");
  FABRIC_CHECK_EQ(json_string_or(*status.find("report"), "lifecycle", ""), std::string("retired"));
}

FABRIC_TEST(node_process, damaged_durable_state_stops_the_boot) {
  test::TempDir directory("node-corrupt");
  test::NodeLaunchOptions options;
  options.state_directory = directory.sub("state");
  options.component = "node-a";
  test::NodeProcess node = test::launch_node(options);
  FABRIC_CHECK(node.process->terminate());

  const std::string state_path = options.state_directory + "/state.fxi";
  FABRIC_CHECK(path_exists(state_path));
  {
    std::FILE* handle = std::fopen(state_path.c_str(), "r+b");
    FABRIC_CHECK(handle != nullptr);
    std::fseek(handle, -2, SEEK_END);
    std::fputc(0x5A, handle);
    std::fclose(handle);
  }
  // Removing the backup forces the conservative path: refuse rather than guess.
  std::remove((state_path + ".prev").c_str());
  auto restarted = test::ChildProcess::spawn(FABRIC_EVOLUTION_NODE_BINARY,
                                             {"--component", "node-a", "--shard", "shard-0",
                                              "--state-dir", options.state_directory});
  FABRIC_CHECK_OK(restarted);
  const std::string output = restarted.value().drain();
  const int code = restarted.value().wait();
  FABRIC_CHECK_EQ(code, 3);
  FABRIC_CHECK(output.find("damaged") != std::string::npos);
}

FABRIC_TEST(node_process, damaged_identity_stops_the_boot) {
  test::TempDir directory("node-identity");
  test::NodeLaunchOptions options;
  options.state_directory = directory.sub("state");
  options.component = "node-a";
  test::NodeProcess node = test::launch_node(options);
  FABRIC_CHECK(node.process->terminate());
  const std::string identity_path = options.state_directory + "/identity.fxi";
  {
    std::FILE* handle = std::fopen(identity_path.c_str(), "r+b");
    FABRIC_CHECK(handle != nullptr);
    std::fseek(handle, 40, SEEK_SET);
    std::fputc(0x00, handle);
    std::fclose(handle);
  }
  auto restarted = test::ChildProcess::spawn(FABRIC_EVOLUTION_NODE_BINARY,
                                             {"--component", "node-a", "--shard", "shard-0",
                                              "--state-dir", options.state_directory});
  FABRIC_CHECK_OK(restarted);
  const std::string output = restarted.value().drain();
  FABRIC_CHECK_EQ(restarted.value().wait(), 3);
  FABRIC_CHECK(output.find("identity") != std::string::npos);
}

FABRIC_TEST(node_process, snapshot_install_rejects_a_corrupted_image) {
  NodeFixture source_fixture;
  source_fixture.grant();
  Json write = Json::object();
  write.set("claim", to_json(source_fixture.claim()));
  write.set("key", Json("region"));
  write.set("value", Json("eu-west"));
  test::must_node(source_fixture.node.port, "node.write", write);
  const Json snapshot = test::must_node(source_fixture.node.port, "node.snapshot",
                                        Json::object({{"handoff", Json(1)}}));
  const Json& image = *snapshot.find("snapshot");

  NodeFixture target_fixture("2.0.0", "1.1", 1, "node-b");
  Json prepared = Json::object();
  prepared.set("handoff", Json(1));
  prepared.set("role", Json("successor"));
  prepared.set("manifest_digest", Json(sha256("manifest").to_hex()));
  prepared.set("target_software", Json("2.0.0"));
  prepared.set("target_protocol", Json("1.1"));
  prepared.set("target_schema", Json(1));
  Json binding = Json::object();
  binding.set("incarnation", to_json(source_fixture.incarnation()));
  binding.set("epoch", Json(1));
  binding.set("generation", Json(1));
  binding.set("token", Json(source_fixture.token.to_compact_string()));
  binding.set("schema", Json(1));
  binding.set("handoff", Json(1));
  prepared.set("source", std::move(binding));
  test::must_node(target_fixture.node.port, "node.prepare", std::move(prepared));

  Json install = Json::object();
  install.set("handoff", Json(1));
  install.set("snapshot", image);
  test::must_node(target_fixture.node.port, "node.install_snapshot", install);

  Json corrupted = install;
  Json tampered = image;
  tampered.set("fields", Json::object({{"region", Json("us-east")}}));
  corrupted.set("snapshot", std::move(tampered));
  FABRIC_CHECK_ERR(test::node_call(target_fixture.node.port, "node.install_snapshot", corrupted),
                   ErrorCode::IntegrityFailure);

  Json before_prepare = install;
  NodeFixture unprepared_fixture("2.0.0", "1.1", 1, "node-c");
  FABRIC_CHECK_ERR(test::node_call(unprepared_fixture.node.port, "node.install_snapshot", before_prepare),
                   ErrorCode::IllegalTransition);
}

FABRIC_TEST(node_process, unknown_operations_are_refused) {
  NodeFixture fixture;
  FABRIC_CHECK_ERR(test::node_call(fixture.node.port, "node.nonsense"), ErrorCode::NotFound);
  FABRIC_CHECK_ERR(test::node_call(fixture.node.port, "node.write", Json::object()),
                   ErrorCode::InvalidArgument);
  FABRIC_CHECK_ERR(test::node_call(fixture.node.port, "node.read", Json::object()),
                   ErrorCode::InvalidArgument);
}

FABRIC_TEST(node_process, repeated_open_close_cycles_are_stable) {
  test::TempDir directory("node-cycles");
  test::NodeLaunchOptions options;
  options.state_directory = directory.sub("state");
  options.component = "node-a";
  for (int cycle = 0; cycle < 6; ++cycle) {
    test::NodeProcess node = test::launch_node(options);
    const Json status = test::must_node(node.port, "node.status");
    FABRIC_CHECK_EQ(json_u64_or(*status.find("report"), "lsn", 0), static_cast<std::uint64_t>(0));
    FABRIC_CHECK(node.process->terminate());
  }
}

}  // namespace fabric::evolution
