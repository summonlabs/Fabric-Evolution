// Fabric Evolution — command line interface.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Drives a running evolution controller over its admin endpoint. Every command
// prints either a deterministic human summary or, with --json, the exact
// response document (including the decision explanation the controller built).

#include <cstdio>
#include <string>
#include <vector>

#include "args.hpp"
#include "fabric/evolution/controller.hpp"

namespace {

using fabric::evolution::Json;
using fabric::evolution::RpcRequest;
using fabric::evolution::RpcResponse;
using fabric::evolution::ServerOptions;

void usage() {
  std::printf(
      "fabric-evolution — controlled control-plane evolution\n"
      "\n"
      "usage: fabric-evolution --controller <host:port> [--json] <command> [options]\n"
      "\n"
      "commands:\n"
      "  plan --manifest <file>   admit an evolution manifest (sealed and digest checked)\n"
      "  preflight                compatibility, protocol and migration preflight\n"
      "  start                    run the handoff to completion\n"
      "  advance                  perform exactly one handoff phase\n"
      "  pause                    pause at the next safe phase boundary\n"
      "  resume                   resume a paused campaign\n"
      "  abort                    roll back, or continue forward when rollback is unsafe\n"
      "  reconcile                reconcile controller state with component reports\n"
      "  status                   campaign, handoff, window and authority summary\n"
      "  handoff                  handoff record and phase history\n"
      "  epoch                    durable epoch journal\n"
      "  authority                per-shard authority leases and fences\n"
      "  explain --topic <topic>  deterministic explanation "
      "(campaign|authority|handoff|epoch|window|compatibility)\n"
      "\n"
      "options:\n"
      "  --controller <host:port> admin endpoint (required)\n"
      "  --json                   print the raw response document\n"
      "  --deadline-ms <n>        per-request socket deadline (default 30000)\n");
}

[[nodiscard]] bool split_endpoint(const std::string& text, std::string& host, std::uint16_t& port) {
  const std::size_t colon = text.rfind(':');
  if (colon == std::string::npos || colon == 0) {
    return false;
  }
  host = text.substr(0, colon);
  try {
    const unsigned long parsed = std::stoul(text.substr(colon + 1));
    if (parsed == 0 || parsed > 65535) {
      return false;
    }
    port = static_cast<std::uint16_t>(parsed);
  } catch (const std::exception&) {
    return false;
  }
  return true;
}

void print_explanation(const Json& body) {
  const Json* explanation = body.find("explanation");
  if (explanation == nullptr || !explanation->is_object()) {
    return;
  }
  std::printf("decision: %s [%s] policy=%s\n",
              fabric::evolution::json_string_or(*explanation, "decision", "?").c_str(),
              fabric::evolution::json_string_or(*explanation, "outcome", "?").c_str(),
              fabric::evolution::json_string_or(*explanation, "policy", "?").c_str());
  const std::string action = fabric::evolution::json_string_or(*explanation, "selected_action", "");
  if (!action.empty()) {
    std::printf("  action: %s\n", action.c_str());
  }
  const std::string state = fabric::evolution::json_string_or(*explanation, "resulting_state", "");
  if (!state.empty()) {
    std::printf("  resulting state: %s\n", state.c_str());
  }
  if (const Json* notes = explanation->find("notes")) {
    for (std::size_t index = 0; index < notes->size(); ++index) {
      std::printf("  note: %s\n", notes->at(index).try_string()->c_str());
    }
  }
  if (const Json* rejected = explanation->find("rejected_alternatives")) {
    for (std::size_t index = 0; index < rejected->size(); ++index) {
      const Json& entry = rejected->at(index);
      std::printf("  rejected %s: %s\n",
                  fabric::evolution::json_string_or(entry, "action", "?").c_str(),
                  fabric::evolution::json_string_or(entry, "reason", "?").c_str());
    }
  }
}

void print_status(const Json& body) {
  if (const Json* campaign = body.find("campaign")) {
    std::printf("campaign: %s state=%s revision=%llu epoch=%llu generation=%llu\n",
                fabric::evolution::json_string_or(*campaign, "id", "?").c_str(),
                fabric::evolution::json_string_or(*campaign, "state", "?").c_str(),
                static_cast<unsigned long long>(fabric::evolution::json_u64_or(*campaign, "revision", 0)),
                static_cast<unsigned long long>(fabric::evolution::json_u64_or(*campaign, "epoch", 0)),
                static_cast<unsigned long long>(
                    fabric::evolution::json_u64_or(*campaign, "generation", 0)));
  }
  if (const Json* handoff = body.find("handoff")) {
    std::printf("handoff: id=%llu phase=%s attempt=%llu predecessor=%s successor=%s\n",
                static_cast<unsigned long long>(fabric::evolution::json_u64_or(*handoff, "id", 0)),
                fabric::evolution::json_string_or(*handoff, "phase", "?").c_str(),
                static_cast<unsigned long long>(fabric::evolution::json_u64_or(*handoff, "attempt", 0)),
                fabric::evolution::json_string_or(*handoff, "predecessor", "?").c_str(),
                fabric::evolution::json_string_or(*handoff, "successor", "?").c_str());
  }
  if (const Json* window = body.find("window")) {
    std::printf("window: open=%s operations=%llu/%llu elapsed_ms=%llu/%llu exhausted=%s\n",
                fabric::evolution::json_bool_or(*window, "open", false) ? "true" : "false",
                static_cast<unsigned long long>(fabric::evolution::json_u64_or(*window, "operations", 0)),
                static_cast<unsigned long long>(
                    fabric::evolution::json_u64_or(*window, "operation_limit", 0)),
                static_cast<unsigned long long>(fabric::evolution::json_u64_or(*window, "elapsed_ms", 0)),
                static_cast<unsigned long long>(
                    fabric::evolution::json_u64_or(*window, "duration_limit_ms", 0)),
                fabric::evolution::json_bool_or(*window, "exhausted", false) ? "true" : "false");
  }
  if (const Json* authority = body.find("authority")) {
    for (std::size_t index = 0; index < authority->size(); ++index) {
      const Json& entry = authority->at(index);
      std::printf("authority: shard=%s epoch=%llu generation=%llu mutating_owners=%llu slot_free=%s\n",
                  fabric::evolution::json_string_or(entry, "shard", "?").c_str(),
                  static_cast<unsigned long long>(fabric::evolution::json_u64_or(entry, "epoch", 0)),
                  static_cast<unsigned long long>(fabric::evolution::json_u64_or(entry, "generation", 0)),
                  static_cast<unsigned long long>(
                      fabric::evolution::json_u64_or(entry, "mutating_authority_count", 0)),
                  fabric::evolution::json_bool_or(entry, "mutation_slot_free", true) ? "true" : "false");
    }
  }
  if (const Json* reports = body.find("source_report")) {
    std::printf("source: incarnation=%s lsn=%llu schema=%llu lifecycle=%s\n",
                fabric::evolution::json_string_or(*reports, "component", "?").c_str(),
                static_cast<unsigned long long>(fabric::evolution::json_u64_or(*reports, "lsn", 0)),
                static_cast<unsigned long long>(fabric::evolution::json_u64_or(*reports, "schema", 0)),
                fabric::evolution::json_string_or(*reports, "lifecycle", "?").c_str());
  }
  if (const Json* reports = body.find("target_report")) {
    std::printf("target: incarnation=%s lsn=%llu schema=%llu lifecycle=%s\n",
                fabric::evolution::json_string_or(*reports, "component", "?").c_str(),
                static_cast<unsigned long long>(fabric::evolution::json_u64_or(*reports, "lsn", 0)),
                static_cast<unsigned long long>(fabric::evolution::json_u64_or(*reports, "schema", 0)),
                fabric::evolution::json_string_or(*reports, "lifecycle", "?").c_str());
  }
  const std::string deferral = fabric::evolution::json_string_or(body, "last_deferral_reason", "");
  if (!deferral.empty()) {
    std::printf("last deferral: %s\n", deferral.c_str());
  }
}

}  // namespace

int main(int argc, char** argv) {
  fabric::evolution::tools::Arguments args(
      argc, argv, {"--controller", "--manifest", "--topic", "--deadline-ms"});
  if (argc < 2 || args.has("--help") || args.has("-h")) {
    usage();
    return argc < 2 ? 2 : 0;
  }
  const std::string endpoint_text = args.value_or("--controller", "");
  if (endpoint_text.empty()) {
    std::fprintf(stderr, "error: --controller <host:port> is required\n");
    return 2;
  }
  std::string host;
  std::uint16_t port = 0;
  if (!split_endpoint(endpoint_text, host, port)) {
    std::fprintf(stderr, "error: --controller must be host:port\n");
    return 2;
  }
  const bool json_output = args.has("--json");
  const std::vector<std::string> positional = args.positional();
  if (positional.empty()) {
    usage();
    return 2;
  }
  const std::string command = positional.front();

  ServerOptions options;
  options.io_deadline_ms = args.u64_or("--deadline-ms", 30000);

  RpcRequest request;
  request.id = "cli";
  request.op = command;
  request.body = Json::object();

  if (command == "plan") {
    const std::string manifest_path = args.value_or("--manifest", "");
    if (manifest_path.empty()) {
      std::fprintf(stderr, "error: plan requires --manifest <file>\n");
      return 2;
    }
    auto manifest = fabric::evolution::load_manifest_file(manifest_path);
    if (!manifest.ok()) {
      std::fprintf(stderr, "error: %s\n", manifest.status().to_string().c_str());
      return 3;
    }
    request.body.set("manifest", fabric::evolution::to_json(manifest.value()));
  } else if (command == "explain") {
    const std::string topic = args.value_or("--topic", "");
    if (topic.empty()) {
      std::fprintf(stderr, "error: explain requires --topic <topic>\n");
      return 2;
    }
    request.body.set("topic", Json(topic));
  } else if (command != "preflight" && command != "start" && command != "advance" &&
             command != "pause" && command != "resume" && command != "abort" &&
             command != "reconcile" && command != "status" && command != "handoff" &&
             command != "epoch" && command != "authority") {
    std::fprintf(stderr, "error: unknown command: %s\n", command.c_str());
    usage();
    return 2;
  }

  auto response = fabric::evolution::rpc_call(host, port, request, options);
  if (!response.ok()) {
    std::fprintf(stderr, "error: %s\n", response.status().to_string().c_str());
    return 4;
  }
  const RpcResponse& reply = response.value();
  if (json_output) {
    std::printf("%s\n", reply.body.dump_pretty().c_str());
  } else if (!reply.status.ok()) {
    std::fprintf(stderr, "refused: %s\n", reply.status.to_string().c_str());
    if (!reply.body.is_null()) {
      print_explanation(reply.body);
    }
  } else if (command == "status" || command == "start" || command == "resume" ||
             command == "advance" || command == "pause" || command == "reconcile" ||
             command == "abort") {
    print_status(reply.body);
    print_explanation(reply.body);
  } else if (command == "plan" || command == "preflight") {
    print_status(reply.body);
    print_explanation(reply.body);
  } else {
    std::printf("%s\n", reply.body.dump_pretty().c_str());
  }
  return reply.status.ok() ? 0 : 1;
}
