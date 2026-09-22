// Fabric Evolution — evolution controller process.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Runs the controller as an independent OS process with a durable state
// directory and an admin TCP endpoint that the fabric-evolution CLI drives.

#include <chrono>
#include <csignal>
#include <cstdio>
#include <string>
#include <thread>

#include "args.hpp"
#include "fabric/evolution/controller.hpp"

namespace {

volatile std::sig_atomic_t g_stop = 0;

extern "C" void on_signal(int) { g_stop = 1; }

void usage() {
  std::printf(
      "fabric-evolution-controller — Fabric OS evolution controller\n"
      "\n"
      "usage: fabric-evolution-controller --state-dir <path> [options]\n"
      "\n"
      "  --state-dir <path>       durable controller state directory (required)\n"
      "  --id <id>                controller identity (default evolution-controller)\n"
      "  --host <addr>            admin bind address (default 127.0.0.1)\n"
      "  --port <n>               admin bind port, 0 selects ephemeral (default 0)\n"
      "  --compat <path>          compatibility registry JSON document\n"
      "  --evidence-policy <p>    exact | refresh (default exact)\n"
      "  --lease-ttl-ms <n>       authority lease lifetime (default 6000)\n"
      "  --run-ms <n>             exit after n milliseconds instead of serving until killed\n");
}

}  // namespace

int main(int argc, char** argv) {
  fabric::evolution::tools::Arguments args(
      argc, argv,
      {"--state-dir", "--id", "--host", "--port", "--compat", "--evidence-policy", "--lease-ttl-ms",
       "--run-ms"});
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
  auto controller_id = ComponentId::parse(args.value_or("--id", "evolution-controller"));
  if (!controller_id.has_value()) {
    std::fprintf(stderr, "error: invalid controller id\n");
    return 2;
  }

  StaticCompatibilityRegistry fallback_registry;
  JsonCompatibilityRegistry file_registry;
  CompatibilityRegistry* registry = &fallback_registry;
  const std::string compat_path = args.value_or("--compat", "");
  if (!compat_path.empty()) {
    auto loaded = JsonCompatibilityRegistry::load_file(compat_path);
    if (!loaded.ok()) {
      std::fprintf(stderr, "error: %s\n", loaded.status().to_string().c_str());
      return 3;
    }
    file_registry = loaded.take();
    registry = &file_registry;
  }

  ControllerOptions options;
  options.controller_id = *controller_id;
  options.state_directory = state_directory;
  options.host = args.value_or("--host", "127.0.0.1");
  options.port = args.u16_or("--port", 0);
  const std::string policy = args.value_or("--evidence-policy", "exact");
  if (policy == "refresh") {
    options.evidence_policy = EvidencePolicy::AllowRefreshIfVerdictUnchanged;
  } else if (policy == "exact") {
    options.evidence_policy = EvidencePolicy::RequireExactMatch;
  } else {
    std::fprintf(stderr, "error: --evidence-policy must be exact or refresh\n");
    return 2;
  }
  const std::uint64_t lease_ttl = args.u64_or("--lease-ttl-ms", 6000);
  if (lease_ttl == 0) {
    std::fprintf(stderr, "error: --lease-ttl-ms must be positive\n");
    return 2;
  }
  options.lease_ttl_ms = lease_ttl;

  SystemClock clock;
  EvolutionController controller(options, *registry, clock);
  const Status opened = controller.open();
  if (!opened.ok()) {
    std::fprintf(stderr, "error: %s\n", opened.to_string().c_str());
    return 3;
  }
  const Status started = controller.start_admin();
  if (!started.ok()) {
    std::fprintf(stderr, "error: %s\n", started.to_string().c_str());
    return 3;
  }

  std::printf("fabric-evolution-controller ready controller=%s port=%u registry=%s\n",
              options.controller_id.str().c_str(), static_cast<unsigned>(controller.port()),
              registry->name().c_str());
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
  controller.stop();
  std::printf("fabric-evolution-controller stopped\n");
  std::fflush(stdout);
  return 0;
}
