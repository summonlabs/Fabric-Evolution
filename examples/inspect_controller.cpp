// Fabric Evolution — inspecting a running controller.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// A minimal client of a live controller: it prints the campaign, the handoff
// phase, the authority picture and the deterministic explanation of the current
// mixed-version window. Run a controller and use this to look inside it.

#include <cstdio>
#include <string>

#include "fabric/evolution/controller.hpp"

int main(int argc, char** argv) {
  using namespace fabric::evolution;
  if (argc < 3) {
    std::printf("usage: %s <host> <port>\n", argv[0]);
    return 2;
  }
  const std::string host = argv[1];
  const std::uint16_t port = static_cast<std::uint16_t>(std::stoul(argv[2]));

  ServerOptions options;
  options.io_deadline_ms = 5000;

  const auto query = [&](const std::string& op, Json body) -> Result<Json> {
    RpcRequest request;
    request.id = "example";
    request.op = op;
    request.body = std::move(body);
    auto response = rpc_call(host, port, request, options);
    if (!response.ok()) {
      return response.status();
    }
    if (!response.value().status.ok()) {
      return response.value().status;
    }
    return response.value().body;
  };

  auto status = query("status", Json::object());
  if (!status.ok()) {
    std::fprintf(stderr, "controller query failed: %s\n", status.status().to_string().c_str());
    return 1;
  }
  std::printf("status:\n%s\n", status.value().dump_pretty().c_str());

  auto authority = query("authority", Json::object());
  if (authority.ok()) {
    std::printf("authority:\n%s\n", authority.value().dump_pretty().c_str());
  }
  auto window = query("explain", Json::object({{"topic", Json("window")}}));
  if (window.ok()) {
    std::printf("mixed-version window:\n%s\n", window.value().dump_pretty().c_str());
  }
  auto compatibility = query("explain", Json::object({{"topic", Json("compatibility")}}));
  if (compatibility.ok()) {
    std::printf("compatibility evidence:\n%s\n", compatibility.value().dump_pretty().c_str());
  }
  return 0;
}
