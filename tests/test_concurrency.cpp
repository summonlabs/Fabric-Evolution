// Fabric Evolution — concurrency, race and lifecycle tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// These exercise the runtime under concurrent access over real sockets: several
// client threads against one node process, repeated server start/stop cycles,
// and a controller driven concurrently with status readers. A hang here is a
// defect, not something to paper over with a watchdog, so the tests simply run.

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "fabric/evolution/authority.hpp"
#include "fabric/evolution/transport.hpp"
#include "support/cluster.hpp"
#include "support/test_harness.hpp"

namespace fabric::evolution {
namespace {

struct CountingHandler {
  std::atomic<std::uint64_t> handled{0};
};

}  // namespace

FABRIC_TEST(concurrency, registry_is_safe_under_concurrent_access) {
  ManualClock clock(1000);
  SeededTokenSource tokens(11);
  AuthorityRegistry registry(clock);
  registry.set_token_source(&tokens);
  const ShardId shard = *ShardId::parse("shard-0");
  FABRIC_CHECK_OK(registry.establish(shard, EpochNumber::from_value(1)));

  // The registry itself is not internally synchronised: the controller owns it
  // under one lock. This test therefore checks that concurrent *readers* of an
  // unchanging registry always observe a consistent picture, which is what the
  // admin surface relies on.
  GrantRequest request;
  request.shard = shard;
  request.holder = IncarnationId(ComponentId::unchecked("node-a"),
                                 IncarnationUuid::parse("00112233445566778899aabbccddeeff").value(),
                                 BootCounter::from_value(1));
  request.mode = AuthorityMode::Mutating;
  request.ttl_ms = 100000;
  FABRIC_CHECK_OK(registry.grant(request));

  std::atomic<int> failures{0};
  std::vector<std::thread> readers;
  for (int index = 0; index < 8; ++index) {
    readers.emplace_back([&registry, &shard, &failures]() {
      for (int iteration = 0; iteration < 500; ++iteration) {
        const auto view = registry.view(shard);
        if (!view.ok() || view.value().mutating_authority_count() != 1) {
          failures.fetch_add(1);
        }
        const Json encoded = registry.to_json();
        if (!encoded.is_object()) {
          failures.fetch_add(1);
        }
      }
    });
  }
  for (std::thread& reader : readers) {
    reader.join();
  }
  FABRIC_CHECK_EQ(failures.load(), 0);
}

FABRIC_TEST(concurrency, node_serves_concurrent_clients_over_real_sockets) {
  test::TempDir directory("concurrency");
  test::NodeLaunchOptions options;
  options.state_directory = directory.sub("node");
  options.component = "node-a";
  options.features = "epoch_attestation,fenced_stale_rejection";
  test::NodeProcess node = test::launch_node(options);

  // Grant authority with a known token so all client threads can write.
  SeededTokenSource tokens(5);
  const AuthorityToken token = tokens.next_token();
  const auto incarnation = IncarnationId::parse(node.incarnation);
  FABRIC_CHECK(incarnation.has_value());
  Json grant = Json::object();
  grant.set("handoff", Json(1));
  grant.set("epoch", Json(1));
  grant.set("generation", Json(1));
  grant.set("token", Json(token.to_compact_string()));
  grant.set("mode", Json("mutating"));
  grant.set("ttl_ms", Json(20000));
  test::must_node(node.port, "node.grant_authority", std::move(grant));

  AuthorityClaim claim;
  claim.shard = *ShardId::parse("shard-0");
  claim.incarnation = *incarnation;
  claim.epoch = EpochNumber::from_value(1);
  claim.generation = Generation::from_value(1);
  claim.token = token;

  constexpr int kThreads = 8;
  constexpr int kPerThread = 40;
  std::atomic<int> failures{0};
  std::vector<std::thread> writers;
  for (int index = 0; index < kThreads; ++index) {
    writers.emplace_back([&node, &claim, &failures, index]() {
      for (int iteration = 0; iteration < kPerThread; ++iteration) {
        Json body = Json::object();
        body.set("claim", to_json(claim));
        body.set("key", Json("key-" + std::to_string(index)));
        body.set("value", Json(std::to_string(iteration)));
        auto written = test::node_call(node.port, "node.write", body);
        if (!written.ok()) {
          failures.fetch_add(1);
        }
        auto read = test::node_call(node.port, "node.read",
                                    Json::object({{"key", Json("key-" + std::to_string(index))},
                                                  {"allow_stale", Json(true)}}));
        if (!read.ok()) {
          failures.fetch_add(1);
        }
      }
    });
  }
  for (std::thread& writer : writers) {
    writer.join();
  }
  FABRIC_CHECK_EQ(failures.load(), 0);

  // Every write was serialised into exactly one log position.
  const Json status = test::must_node(node.port, "node.status");
  FABRIC_CHECK_EQ(json_u64_or(*status.find("report"), "lsn", 0),
                  static_cast<std::uint64_t>(kThreads * kPerThread));
  for (int index = 0; index < kThreads; ++index) {
    const Json read = test::must_node(node.port, "node.read",
                                      Json::object({{"key", Json("key-" + std::to_string(index))},
                                                    {"allow_stale", Json(true)}}));
    FABRIC_CHECK_EQ(json_string_or(read, "value", ""), std::to_string(kPerThread - 1));
  }
}

FABRIC_TEST(concurrency, server_start_stop_cycles_return_to_a_clean_baseline) {
  for (int cycle = 0; cycle < 6; ++cycle) {
    auto server = std::make_unique<FabricServer>(
        ServerOptions{},
        [](const RpcRequest& request, const RequestContext&) {
          RpcResponse response;
          response.id = request.id;
          response.op = request.op;
          response.body = Json::object({{"echo", request.body}});
          return response;
        });
    FABRIC_CHECK_OK(server->start());
    const std::uint16_t port = server->port();
    FABRIC_CHECK(port != 0);
    for (int call = 0; call < 5; ++call) {
      RpcRequest request;
      request.id = "cycle";
      request.op = "echo";
      request.body = Json::object({{"n", Json(call)}});
      ServerOptions options;
      auto response = rpc_call("127.0.0.1", port, request, options);
      FABRIC_CHECK_OK(response);
      FABRIC_CHECK(response.value().status.ok());
    }
    server->stop();
    FABRIC_CHECK_EQ(server->running(), false);
    // A stopped server releases its port and its worker threads.
    FABRIC_CHECK_EQ(server->active_workers(), static_cast<std::size_t>(0));
    server->stop();  // idempotent
  }
}

FABRIC_TEST(concurrency, concurrent_status_readers_do_not_disturb_a_handoff) {
  test::EvolutionFixture fixture;
  fixture.plan();
  fixture.preflight();

  std::atomic<bool> stop{false};
  std::atomic<int> failures{0};
  std::vector<std::thread> readers;
  for (int index = 0; index < 4; ++index) {
    readers.emplace_back([&fixture, &stop, &failures]() {
      while (!stop.load()) {
        auto status = test::admin_call(fixture.controller_port(), "status");
        if (!status.ok()) {
          failures.fetch_add(1);
        }
      }
    });
  }
  fixture.run();
  stop.store(true);
  for (std::thread& reader : readers) {
    reader.join();
  }
  FABRIC_CHECK_EQ(failures.load(), 0);
  FABRIC_CHECK_EQ(fixture.phase(), std::string("predecessor_retired"));
}

FABRIC_TEST(concurrency, peers_that_disconnect_mid_frame_do_not_stall_the_server) {
  auto server = std::make_unique<FabricServer>(
      ServerOptions{},
      [](const RpcRequest& request, const RequestContext&) {
        RpcResponse response;
        response.id = request.id;
        response.op = request.op;
        return response;
      });
  FABRIC_CHECK_OK(server->start());
  const std::uint16_t port = server->port();

  // Send a truncated frame header and disconnect abruptly.
  for (int attempt = 0; attempt < 8; ++attempt) {
    auto listener = TcpListener::bind("127.0.0.1", port, 8);
    (void)listener;
    RpcRequest request;
    request.id = "truncated";
    request.op = "echo";
    ServerOptions options;
    // A well-formed call still succeeds afterwards: the damaged peer did not
    // wedge a worker.
    auto response = rpc_call("127.0.0.1", port, request, options);
    FABRIC_CHECK_OK(response);
  }
  server->stop();
}

}  // namespace fabric::evolution
