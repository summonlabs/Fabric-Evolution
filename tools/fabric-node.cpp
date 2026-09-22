// Fabric Evolution — reference control-plane node process.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Runs one replicated control-plane component as an independent OS process with
// a durable state directory and a real TCP endpoint. The process prints a single
// machine-readable readiness line so that a supervisor (or a test harness) can
// discover the ephemeral port it bound.

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "args.hpp"
#include "fabric/evolution/node.hpp"

namespace {

volatile std::sig_atomic_t g_stop = 0;

extern "C" void on_signal(int) { g_stop = 1; }

[[nodiscard]] fabric::evolution::FeatureSet parse_features(const std::string& text) {
  fabric::evolution::FeatureSet set;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t comma = text.find(',', start);
    const std::string name =
        comma == std::string::npos ? text.substr(start) : text.substr(start, comma - start);
    if (!name.empty()) {
      const auto feature = fabric::evolution::feature_from_name(name);
      if (feature.has_value()) {
        set.add(*feature);
      }
    }
    if (comma == std::string::npos) {
      break;
    }
    start = comma + 1;
  }
  return set;
}

void usage() {
  std::printf(
      "fabric-evolution-node — reference Fabric OS control-plane component\n"
      "\n"
      "usage: fabric-evolution-node --component <id> --shard <id> --state-dir <path> [options]\n"
      "\n"
      "  --component <id>       component identity (default node-a)\n"
      "  --shard <id>           shard this component serves (default shard-0)\n"
      "  --state-dir <path>     durable state directory (required)\n"
      "  --host <addr>          bind address (default 127.0.0.1)\n"
      "  --port <n>             bind port, 0 selects an ephemeral port (default 0)\n"
      "  --software <x.y.z>     software version to advertise (default 1.0.0)\n"
      "  --protocol <x.y>       protocol version to advertise (default 1.0)\n"
      "  --schema <n>           initial schema version (default 1)\n"
      "  --features <a,b>       comma separated feature names to advertise\n"
      "  --lease-ttl-ms <n>     lease lifetime used when the controller grants one\n"
      "  --run-ms <n>           exit after n milliseconds instead of serving until killed\n");
}

}  // namespace

int main(int argc, char** argv) {
  fabric::evolution::tools::Arguments args(
      argc, argv,
      {"--component", "--shard", "--state-dir", "--host", "--port", "--software", "--protocol",
       "--schema", "--features", "--lease-ttl-ms", "--run-ms"});
  if (args.has("--help") || args.has("-h")) {
    usage();
    return 0;
  }
  const std::string state_directory = args.value_or("--state-dir", "");
  if (state_directory.empty()) {
    std::fprintf(stderr, "error: --state-dir is required\n");
    return 2;
  }

  using namespace fabric::evolution;
  auto component = ComponentId::parse(args.value_or("--component", "node-a"));
  if (!component.has_value()) {
    std::fprintf(stderr, "error: invalid component id\n");
    return 2;
  }
  auto shard = ShardId::parse(args.value_or("--shard", "shard-0"));
  if (!shard.has_value()) {
    std::fprintf(stderr, "error: invalid shard id\n");
    return 2;
  }
  auto software = SoftwareVersion::parse(args.value_or("--software", "1.0.0"));
  if (!software.has_value()) {
    std::fprintf(stderr, "error: invalid software version\n");
    return 2;
  }
  auto protocol = ProtocolVersion::parse(args.value_or("--protocol", "1.0"));
  if (!protocol.has_value()) {
    std::fprintf(stderr, "error: invalid protocol version\n");
    return 2;
  }
  const std::uint64_t schema = args.u64_or("--schema", 1);
  if (schema == 0 || schema > 0xFFFFFFFFULL) {
    std::fprintf(stderr, "error: schema version out of range\n");
    return 2;
  }

  NodeOptions options;
  options.component = *component;
  options.shard = *shard;
  options.state_directory = state_directory;
  options.host = args.value_or("--host", "127.0.0.1");
  options.port = args.u16_or("--port", 0);
  options.software = *software;
  options.protocol = *protocol;
  options.schema = SchemaVersion::from_value(static_cast<std::uint32_t>(schema));
  options.features = parse_features(args.value_or("--features", ""));
  const std::uint64_t lease_ttl = args.u64_or("--lease-ttl-ms", 6000);
  if (lease_ttl == 0) {
    std::fprintf(stderr, "error: --lease-ttl-ms must be positive\n");
    return 2;
  }
  options.lease_ttl_ms = lease_ttl;

  SystemClock clock;
  ControlPlaneNode node(options, clock);
  const Status opened = node.open();
  if (!opened.ok()) {
    std::fprintf(stderr, "error: %s\n", opened.to_string().c_str());
    return 3;
  }
  const Status started = node.start_server();
  if (!started.ok()) {
    std::fprintf(stderr, "error: %s\n", started.to_string().c_str());
    return 3;
  }

  std::printf("fabric-evolution-node ready component=%s shard=%s port=%u incarnation=%s boot=%llu\n",
              options.component.str().c_str(), options.shard.str().c_str(),
              static_cast<unsigned>(node.port()), node.incarnation().to_string().c_str(),
              static_cast<unsigned long long>(node.incarnation().boot().value()));
  std::fflush(stdout);

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  const std::uint64_t run_ms = args.u64_or("--run-ms", 0);
  const auto started_at = std::chrono::steady_clock::now();
  while (g_stop == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (run_ms != 0) {
      const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - started_at)
                               .count();
      if (static_cast<std::uint64_t>(elapsed) >= run_ms) {
        break;
      }
    }
  }
  node.stop();
  std::printf("fabric-evolution-node stopped component=%s\n", options.component.str().c_str());
  std::fflush(stdout);
  return 0;
}
