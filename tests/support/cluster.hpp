// Fabric Evolution — multi-process cluster harness for the end-to-end proof.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "fabric/evolution/controller.hpp"
#include "fabric/evolution/manifest.hpp"
#include "support/process.hpp"

namespace fabric::evolution::test {

// A scratch directory that removes itself, so a run leaves no debris behind.
class TempDir {
 public:
  explicit TempDir(const std::string& tag);
  ~TempDir();
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  [[nodiscard]] const std::string& path() const noexcept { return path_; }
  [[nodiscard]] std::string sub(const std::string& name) const;

 private:
  std::string path_;
};

struct NodeProcess {
  std::unique_ptr<ChildProcess> process;
  std::string component;
  std::string shard;
  std::string state_directory;
  std::uint16_t port = 0;
  std::string incarnation;
  std::uint64_t boot = 0;

  NodeProcess() = default;
  NodeProcess(NodeProcess&&) noexcept = default;
  NodeProcess& operator=(NodeProcess&&) noexcept = default;
  [[nodiscard]] bool valid() const noexcept { return process != nullptr; }
};

struct ControllerProcess {
  std::unique_ptr<ChildProcess> process;
  std::string state_directory;
  std::uint16_t port = 0;
};

struct NodeLaunchOptions {
  std::string state_directory;
  std::string component = "node-a";
  std::string shard = "shard-0";
  std::string software = "1.0.0";
  std::string protocol = "1.0";
  std::uint32_t schema = 1;
  std::string features;
  std::uint64_t lease_ttl_ms = 8000;
  // A restarted component keeps the address its manifest declares.
  std::uint16_t port = 0;
};

[[nodiscard]] NodeProcess launch_node(const NodeLaunchOptions& options);
[[nodiscard]] ControllerProcess launch_controller(const std::string& state_directory,
                                                 const std::string& compatibility_file,
                                                 std::uint64_t lease_ttl_ms,
                                                 const std::string& evidence_policy = "exact",
                                                 std::uint16_t port = 0);

// Admin calls against a running controller.
[[nodiscard]] Result<Json> admin_call(std::uint16_t port, const std::string& op, Json body = Json::object());
[[nodiscard]] Result<Json> node_call(std::uint16_t port, const std::string& op,
                                     Json body = Json::object());
// Same, but fails the test with a readable message when the call is refused.
// The result may be ignored when a test only cares that the call succeeded.
Json must_admin(std::uint16_t port, const std::string& op, Json body = Json::object());
Json must_node(std::uint16_t port, const std::string& op, Json body = Json::object());

struct CliResult {
  int exit_code = -1;
  std::string output;
};

[[nodiscard]] CliResult run_cli(const std::vector<std::string>& arguments);

// Builds a compatibility decision compatible with the given specs.
[[nodiscard]] CompatibilityDecision make_decision(const ComponentSpec& source, const ComponentSpec& target,
                                                  bool rollback_permitted = true,
                                                  bool unsupported = false);

// Builds a manifest for the given endpoints. The returned manifest is sealed.
[[nodiscard]] EvolutionManifest make_manifest(const ComponentSpec& source, const ComponentSpec& target,
                                              const CompatibilityDecision& decision,
                                              const std::string& campaign = "campaign-1",
                                              bool irreversible = false,
                                              std::uint64_t max_operations = 1000000,
                                              std::uint64_t max_duration_ms = 3600000);

[[nodiscard]] ComponentSpec make_spec(const std::string& component, const std::string& software,
                                      const std::string& protocol, std::uint32_t schema,
                                      std::uint16_t port, FeatureSet features);

[[nodiscard]] FeatureSet standard_features();

// Writes a compatibility registry document that a controller process can load.
// Returns the path for convenience; the result may be ignored.
std::string write_compatibility_registry(const std::string& path,
                                         const std::vector<CompatibilityDecision>& decisions);
// Writes a sealed manifest document for the CLI to admit.
std::string write_manifest_file(const std::string& path, const EvolutionManifest& manifest);

// A complete evolution cluster: a predecessor node, a successor node, a
// controller and a sealed manifest bound to their real ephemeral ports.
struct FixtureOptions {
  bool irreversible = false;
  std::uint64_t max_operations = 1000000;
  std::uint64_t max_duration_ms = 3600000;
  std::uint64_t lease_ttl_ms = 8000;
  std::string source_software = "1.0.0";
  std::string source_protocol = "1.0";
  std::string target_software = "2.0.0";
  std::string target_protocol = "1.1";
  std::uint32_t target_schema = 2;
  bool target_supports_catch_up = true;
  bool plan_on_construction = false;
  std::string campaign = "campaign-e2e";
};

class EvolutionFixture {
 public:
  explicit EvolutionFixture(const FixtureOptions& options = FixtureOptions{});
  ~EvolutionFixture();

  EvolutionFixture(const EvolutionFixture&) = delete;
  EvolutionFixture& operator=(const EvolutionFixture&) = delete;

  [[nodiscard]] const std::string& root() const noexcept { return directory_.path(); }
  [[nodiscard]] std::uint16_t controller_port() const noexcept { return controller_.port; }
  [[nodiscard]] std::uint16_t source_port() const noexcept { return source_.port; }
  [[nodiscard]] std::uint16_t target_port() const noexcept { return target_.port; }
  [[nodiscard]] const EvolutionManifest& manifest() const noexcept { return manifest_; }
  [[nodiscard]] const NodeProcess& source() const noexcept { return source_; }
  [[nodiscard]] const NodeProcess& target() const noexcept { return target_; }

  [[nodiscard]] std::string phase() const;
  [[nodiscard]] std::string campaign_state() const;
  [[nodiscard]] Json status() const;

  void plan();
  void preflight();
  void advance();
  void advance_until(const std::string& wanted_phase);
  void run();

  void restart_source();
  void restart_target();
  void restart_controller();
  void kill_source();
  void kill_target();
  void kill_controller();

  // Client traffic helpers. Results may be ignored where a test only needs the
  // write to have succeeded.
  Json write_to(const NodeProcess& node, const std::string& key, const std::string& value);
  [[nodiscard]] Json read_from(const NodeProcess& node, const std::string& key, bool allow_stale = true);
  [[nodiscard]] Result<Json> try_write_to(const NodeProcess& node, const std::string& key,
                                          const std::string& value);
  [[nodiscard]] AuthorityClaim claim_of(const NodeProcess& node);

 private:
  void launch_source(const NodeLaunchOptions& base);
  void launch_target(const NodeLaunchOptions& base);

  FixtureOptions options_;
  TempDir directory_{"e2e"};
  NodeProcess source_;
  NodeProcess target_;
  ControllerProcess controller_;
  EvolutionManifest manifest_;
};

}  // namespace fabric::evolution::test
