// Fabric Evolution — multi-process cluster harness (implementation).
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "support/cluster.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <random>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "support/test_harness.hpp"

#ifndef FABRIC_EVOLUTION_NODE_BINARY
#error "FABRIC_EVOLUTION_NODE_BINARY must be defined by the build system"
#endif
#ifndef FABRIC_EVOLUTION_CONTROLLER_BINARY
#error "FABRIC_EVOLUTION_CONTROLLER_BINARY must be defined by the build system"
#endif
#ifndef FABRIC_EVOLUTION_CLI_BINARY
#error "FABRIC_EVOLUTION_CLI_BINARY must be defined by the build system"
#endif

namespace fabric::evolution::test {
namespace {

std::atomic<std::uint64_t> g_counter{0};

// Scratch directories must be unique across concurrently running test processes,
// not merely within one: two processes that shared a directory would delete each
// other's durable state mid-test. The process id and a random per-process value
// are therefore part of the name.
[[nodiscard]] std::uint64_t process_identifier() {
#ifdef _WIN32
  return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

[[nodiscard]] std::uint64_t process_nonce() {
  static const std::uint64_t nonce = []() {
    std::random_device device;
    return (static_cast<std::uint64_t>(device()) << 32U) ^ static_cast<std::uint64_t>(device());
  }();
  return nonce;
}

[[nodiscard]] std::string unique_name(const std::string& tag) {
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  const std::uint64_t sequence = g_counter.fetch_add(1);
  return tag + "-" + std::to_string(process_identifier()) + "-" + std::to_string(process_nonce()) +
         "-" + std::to_string(now) + "-" + std::to_string(sequence);
}

}  // namespace

TempDir::TempDir(const std::string& tag) {
  std::error_code error;
  const std::filesystem::path base = std::filesystem::temp_directory_path(error);
  const std::filesystem::path directory =
      (error ? std::filesystem::path(".") : base) / "fabric-evolution-tests" / unique_name(tag);
  std::filesystem::create_directories(directory, error);
  path_ = directory.string();
}

TempDir::~TempDir() {
  std::error_code error;
  std::filesystem::remove_all(path_, error);
}

std::string TempDir::sub(const std::string& name) const { return path_ + "/" + name; }

NodeProcess launch_node(const NodeLaunchOptions& options) {
  std::vector<std::string> arguments = {
      "--component", options.component,
      "--shard",     options.shard,
      "--state-dir", options.state_directory,
      "--host",      "127.0.0.1",
      "--port",      std::to_string(options.port),
      "--software",  options.software,
      "--protocol",  options.protocol,
      "--schema",    std::to_string(options.schema),
      "--lease-ttl-ms", std::to_string(options.lease_ttl_ms),
  };
  if (!options.features.empty()) {
    arguments.push_back("--features");
    arguments.push_back(options.features);
  }
  auto process = ChildProcess::spawn(FABRIC_EVOLUTION_NODE_BINARY, arguments);
  if (!process.ok()) {
    FABRIC_FAIL("cannot start node process: " + process.status().to_string());
  }
  NodeProcess node;
  node.process = std::make_unique<ChildProcess>(process.take());
  node.component = options.component;
  node.shard = options.shard;
  node.state_directory = options.state_directory;
  const auto line = node.process->read_line();
  if (!line.has_value()) {
    FABRIC_FAIL("node process produced no readiness line");
  }
  const auto port = parse_port(*line);
  if (!port.has_value()) {
    FABRIC_FAIL("node readiness line has no port: " + *line);
  }
  node.port = *port;
  node.incarnation = parse_field(*line, "incarnation");
  const std::string boot = parse_field(*line, "boot");
  node.boot = boot.empty() ? 0 : std::stoull(boot);
  if (!node.process->running()) {
    const std::string remainder = node.process->drain();
    FABRIC_FAIL("node process exited immediately after readiness (exit " +
                std::to_string(node.process->exit_code()) + "): " + remainder);
  }
  return node;
}

ControllerProcess launch_controller(const std::string& state_directory,
                                    const std::string& compatibility_file,
                                    std::uint64_t lease_ttl_ms,
                                    const std::string& evidence_policy,
                                    std::uint16_t port) {
  std::vector<std::string> arguments = {
      "--state-dir", state_directory,
      "--host", "127.0.0.1",
      "--port", std::to_string(port),
      "--lease-ttl-ms", std::to_string(lease_ttl_ms),
      "--evidence-policy", evidence_policy,
  };
  if (!compatibility_file.empty()) {
    arguments.push_back("--compat");
    arguments.push_back(compatibility_file);
  }
  auto process = ChildProcess::spawn(FABRIC_EVOLUTION_CONTROLLER_BINARY, arguments);
  if (!process.ok()) {
    FABRIC_FAIL("cannot start controller process: " + process.status().to_string());
  }
  ControllerProcess controller;
  controller.process = std::make_unique<ChildProcess>(process.take());
  controller.state_directory = state_directory;
  const auto line = controller.process->read_line();
  if (!line.has_value()) {
    FABRIC_FAIL("controller process produced no readiness line");
  }
  const auto bound_port = parse_port(*line);
  if (!bound_port.has_value()) {
    FABRIC_FAIL("controller readiness line has no port: " + *line);
  }
  controller.port = *bound_port;
  // A process that printed its readiness line and then died is a real failure;
  // surface what it said instead of letting the next call fail with a bare
  // connection error.
  if (!controller.process->running()) {
    const std::string remainder = controller.process->drain();
    FABRIC_FAIL("controller process exited immediately after readiness (exit " +
                std::to_string(controller.process->exit_code()) + "): " + remainder);
  }
  return controller;
}

Result<Json> admin_call(std::uint16_t port, const std::string& op, Json body) {
  RpcRequest request;
  request.id = "test";
  request.op = op;
  request.body = std::move(body);
  ServerOptions options;
  options.io_deadline_ms = 30000;
  auto response = rpc_call("127.0.0.1", port, request, options);
  if (!response.ok()) {
    return response.status();
  }
  if (!response.value().status.ok()) {
    return response.value().status;
  }
  return response.value().body;
}

Result<Json> node_call(std::uint16_t port, const std::string& op, Json body) {
  return admin_call(port, op, std::move(body));
}

Json must_admin(std::uint16_t port, const std::string& op, Json body) {
  auto result = admin_call(port, op, std::move(body));
  if (!result.ok()) {
    FABRIC_FAIL("admin call " + op + " failed: " + result.status().to_string());
  }
  return result.take();
}

Json must_node(std::uint16_t port, const std::string& op, Json body) {
  auto result = node_call(port, op, std::move(body));
  if (!result.ok()) {
    FABRIC_FAIL("node call " + op + " failed: " + result.status().to_string());
  }
  return result.take();
}

CliResult run_cli(const std::vector<std::string>& arguments) {
  auto process = ChildProcess::spawn(FABRIC_EVOLUTION_CLI_BINARY, arguments);
  if (!process.ok()) {
    FABRIC_FAIL("cannot start the CLI process: " + process.status().to_string());
  }
  CliResult result;
  result.output = process.value().drain();
  result.exit_code = process.value().wait();
  return result;
}

FeatureSet standard_features() {
  FeatureSet set;
  set.add(Feature::SnapshotTransfer)
      .add(Feature::IncrementalCatchUp)
      .add(Feature::ReadOnlySharedAuthority)
      .add(Feature::SchemaMigration)
      .add(Feature::ProtocolNegotiation)
      .add(Feature::ForwardRecovery)
      .add(Feature::ChecksummedRecords)
      .add(Feature::LeaseReattestation)
      .add(Feature::FencedStaleRejection)
      .add(Feature::EpochAttestation)
      .add(Feature::DurableHandoffJournal);
  return set;
}

ComponentSpec make_spec(const std::string& component, const std::string& software,
                        const std::string& protocol, std::uint32_t schema, std::uint16_t port,
                        FeatureSet features) {
  ComponentSpec spec;
  spec.id = *ComponentId::parse(component);
  spec.software = *SoftwareVersion::parse(software);
  spec.protocol = *ProtocolVersion::parse(protocol);
  spec.schema = SchemaVersion::from_value(schema);
  spec.features = features;
  spec.artifact_digest = sha256(software + ":" + component);
  spec.host = "127.0.0.1";
  spec.port = port;
  return spec;
}

CompatibilityDecision make_decision(const ComponentSpec& source, const ComponentSpec& target,
                                    bool rollback_permitted, bool unsupported) {
  CompatibilityDecision decision;
  decision.decision_id = "compat-" + source.software.to_string() + "-" + target.software.to_string();
  decision.registry = "fabric-compatibility-registry";
  decision.subject = source.id;
  decision.source = source.software;
  decision.target = target.software;
  decision.source_protocol = source.protocol;
  decision.target_protocol = target.protocol;
  decision.source_schema = source.schema;
  decision.target_schema = target.schema;
  decision.verdict = unsupported ? CompatibilityVerdict::Unsupported
                                 : CompatibilityVerdict::SupportedWithConstraints;
  decision.rollback_permitted = rollback_permitted;
  decision.max_mixed_version_operations = 1000000;
  decision.max_mixed_version_ms = 3600000;
  decision.certified_features = source.features.intersect(target.features);
  decision.constraints.push_back(CompatibilityConstraint{"mixed_version_window", "bounded"});
  decision.decided_at = EpochNumber::from_value(1);
  const Status sealed = decision.seal();
  if (!sealed.ok()) {
    FABRIC_FAIL("cannot seal the compatibility decision: " + sealed.to_string());
  }
  return decision;
}

EvolutionManifest make_manifest(const ComponentSpec& source, const ComponentSpec& target,
                                const CompatibilityDecision& decision, const std::string& campaign,
                                bool irreversible, std::uint64_t max_operations,
                                std::uint64_t max_duration_ms) {
  EvolutionManifest manifest;
  manifest.id = *ManifestId::parse("manifest-" + campaign);
  manifest.campaign = *CampaignId::parse(campaign);
  manifest.shard = *ShardId::parse("shard-0");
  manifest.revision = Revision::from_value(1);
  manifest.source = source;
  manifest.target = target;
  manifest.compatibility = decision;
  manifest.window.max_operations = max_operations;
  manifest.window.max_duration_ms = max_duration_ms;
  manifest.window.min_participants = 2;
  manifest.protocol.accepted = {source.protocol};
  if (target.protocol != source.protocol) {
    manifest.protocol.accepted.push_back(target.protocol);
  }
  manifest.protocol.required_features.add(Feature::SnapshotTransfer)
      .add(Feature::IncrementalCatchUp)
      .add(Feature::SchemaMigration)
      .add(Feature::ProtocolNegotiation)
      .add(Feature::FencedStaleRejection)
      .add(Feature::EpochAttestation);
  manifest.protocol.allowed_features = decision.certified_features;

  if (source.schema != target.schema) {
    MigrationStepSpec rename_step;
    rename_step.id = *MigrationStepId::parse("step-rename-region");
    rename_step.from = source.schema;
    rename_step.to = SchemaVersion::from_value(source.schema.value() + 1);
    rename_step.function = "rename_field";
    rename_step.parameters.set("from", Json("region"));
    rename_step.parameters.set("to", Json("zone"));
    rename_step.parameters.set("optional", Json("true"));
    rename_step.deterministic = true;
    rename_step.reversible = true;
    rename_step.description = "rename the region field to zone";
    manifest.migrations.push_back(rename_step);

    if (target.schema.value() > source.schema.value() + 1) {
      MigrationStepSpec wrap_step;
      wrap_step.id = *MigrationStepId::parse("step-tag-generation");
      wrap_step.from = rename_step.to;
      wrap_step.to = target.schema;
      wrap_step.function = "wrap_value";
      wrap_step.parameters.set("key", Json("zone"));
      wrap_step.parameters.set("prefix", Json("z:"));
      wrap_step.parameters.set("suffix", Json(":v2"));
      wrap_step.parameters.set("optional", Json("true"));
      wrap_step.deterministic = true;
      wrap_step.reversible = true;
      wrap_step.description = "tag the zone value with its schema generation";
      manifest.migrations.push_back(wrap_step);
    }
  }

  if (irreversible) {
    manifest.migrations.back().irreversible_boundary = true;
    manifest.migrations.back().reversible = false;
    manifest.rollback.kind = RollbackKind::ForwardRecoveryOnly;
    manifest.rollback.recovery_plan =
        "continue forward: the retirement of the field cannot be undone, so the target version "
        "must be completed";
    manifest.rollback.declared_irreversible_boundaries.push_back(manifest.migrations.back().id);
  } else {
    manifest.rollback.kind = RollbackKind::RollbackIfNoBoundaryCrossed;
    manifest.rollback.recovery_plan =
        "restore predecessor authority and retire the successor before any boundary is crossed";
  }
  const Status validation = manifest.validate();
  if (!validation.ok()) {
    FABRIC_FAIL("test manifest is not valid: " + validation.to_string());
  }
  const Status sealed = manifest.seal();
  if (!sealed.ok()) {
    FABRIC_FAIL("cannot seal the test manifest: " + sealed.to_string());
  }
  return manifest;
}

namespace {

const char* const kFixtureFeatures =
    "snapshot_transfer,incremental_catch_up,read_only_shared_authority,schema_migration,"
    "protocol_negotiation,forward_recovery,checksummed_records,lease_reattestation,"
    "fenced_stale_rejection,epoch_attestation,durable_handoff_journal";

}  // namespace

EvolutionFixture::EvolutionFixture(const FixtureOptions& options) : options_(options) {
  NodeLaunchOptions source_options;
  source_options.state_directory = directory_.sub("source-state");
  source_options.component = "node-a";
  source_options.software = options_.source_software;
  source_options.protocol = options_.source_protocol;
  source_options.schema = 1;
  source_options.features = kFixtureFeatures;
  source_options.lease_ttl_ms = options_.lease_ttl_ms;
  source_ = launch_node(source_options);

  NodeLaunchOptions target_options;
  target_options.state_directory = directory_.sub("target-state");
  target_options.component = "node-b";
  target_options.software = options_.target_software;
  target_options.protocol = options_.target_protocol;
  target_options.schema = options_.target_schema;
  target_options.features =
      options_.target_supports_catch_up
          ? kFixtureFeatures
          : "snapshot_transfer,schema_migration,protocol_negotiation,fenced_stale_rejection,"
            "epoch_attestation";
  target_options.lease_ttl_ms = options_.lease_ttl_ms;
  target_ = launch_node(target_options);

  const ComponentSpec source_spec = make_spec("node-a", options_.source_software,
                                              options_.source_protocol, 1, source_.port,
                                              standard_features());
  const ComponentSpec target_spec = make_spec("node-b", options_.target_software,
                                              options_.target_protocol, options_.target_schema,
                                              target_.port, standard_features());
  manifest_ = make_manifest(source_spec, target_spec, make_decision(source_spec, target_spec),
                            options_.campaign, options_.irreversible, options_.max_operations,
                            options_.max_duration_ms);

  const std::string registry_path = write_compatibility_registry(
      directory_.sub("compatibility.json"), {manifest_.compatibility});
  controller_ = launch_controller(directory_.sub("controller-state"), registry_path,
                                  options_.lease_ttl_ms);
  if (options_.plan_on_construction) {
    plan();
  }
}

EvolutionFixture::~EvolutionFixture() = default;

std::string EvolutionFixture::phase() const {
  const Json document = status();
  const Json* handoff = document.find("handoff");
  if (handoff == nullptr) {
    return "none";
  }
  return json_string_or(*handoff, "phase", "none");
}

std::string EvolutionFixture::campaign_state() const {
  const Json document = status();
  const Json* campaign = document.find("campaign");
  if (campaign == nullptr) {
    return "none";
  }
  return json_string_or(*campaign, "state", "none");
}

Json EvolutionFixture::status() const { return must_admin(controller_.port, "status"); }

void EvolutionFixture::plan() {
  (void)must_admin(controller_.port, "plan", Json::object({{"manifest", to_json(manifest_)}}));
}

void EvolutionFixture::preflight() { (void)must_admin(controller_.port, "preflight"); }

void EvolutionFixture::advance() { (void)must_admin(controller_.port, "advance"); }

void EvolutionFixture::run() { (void)must_admin(controller_.port, "run"); }

void EvolutionFixture::advance_until(const std::string& wanted_phase) {
  for (int step = 0; step < 16; ++step) {
    if (phase() == wanted_phase) {
      return;
    }
    (void)must_admin(controller_.port, "advance");
  }
  FABRIC_FAIL("handoff never reached phase " + wanted_phase + " (currently " + phase() + ")");
}

void EvolutionFixture::launch_source(const NodeLaunchOptions& base) {
  NodeLaunchOptions options = base;
  options.state_directory = source_.state_directory;
  options.component = "node-a";
  options.software = options_.source_software;
  options.protocol = options_.source_protocol;
  options.features = kFixtureFeatures;
  options.lease_ttl_ms = options_.lease_ttl_ms;
  options.port = base.port;
  source_ = launch_node(options);
}

void EvolutionFixture::launch_target(const NodeLaunchOptions& base) {
  NodeLaunchOptions options = base;
  options.state_directory = target_.state_directory;
  options.component = "node-b";
  options.software = options_.target_software;
  options.protocol = options_.target_protocol;
  options.schema = options_.target_schema;
  options.features = kFixtureFeatures;
  options.lease_ttl_ms = options_.lease_ttl_ms;
  options.port = base.port;
  target_ = launch_node(options);
}

void EvolutionFixture::kill_source() { (void)source_.process->terminate(); }
void EvolutionFixture::kill_target() { (void)target_.process->terminate(); }
void EvolutionFixture::kill_controller() { (void)controller_.process->terminate(); }

void EvolutionFixture::restart_source() {
  NodeLaunchOptions base;
  base.port = source_.port;
  kill_source();
  launch_source(base);
}

void EvolutionFixture::restart_target() {
  NodeLaunchOptions base;
  base.port = target_.port;
  kill_target();
  launch_target(base);
}

void EvolutionFixture::restart_controller() {
  const std::uint16_t port = controller_.port;
  const std::string state_directory = controller_.state_directory;
  kill_controller();
  const std::string registry_path = directory_.sub("compatibility.json");
  if (!path_exists(registry_path)) {
    write_compatibility_registry(registry_path, {manifest_.compatibility});
  }
  controller_ = launch_controller(state_directory, registry_path, options_.lease_ttl_ms, "exact", port);
}

AuthorityClaim EvolutionFixture::claim_of(const NodeProcess& node) {
  const Json document = must_node(node.port, "node.status");
  const Json& report = *document.find("report");
  AuthorityClaim claim;
  claim.shard = *ShardId::parse("shard-0");
  claim.epoch = EpochNumber::from_value(json_u64_or(report, "epoch", 0));
  claim.generation = Generation::from_value(json_u64_or(report, "generation", 0));
  auto token = AuthorityToken::parse(json_string_or(report, "token", ""));
  if (!token.has_value()) {
    FABRIC_FAIL("node report carries no usable authority token");
  }
  claim.token = *token;
  const Json* incarnation = report.find("incarnation");
  if (incarnation == nullptr) {
    FABRIC_FAIL("node report carries no incarnation");
  }
  auto parsed_incarnation = incarnation_from_json(*incarnation);
  if (!parsed_incarnation.ok()) {
    FABRIC_FAIL("node report incarnation is malformed: " + parsed_incarnation.status().message());
  }
  claim.incarnation = parsed_incarnation.value();
  return claim;
}

Json EvolutionFixture::write_to(const NodeProcess& node, const std::string& key,
                                const std::string& value) {
  Json body = Json::object();
  body.set("claim", to_json(claim_of(node)));
  body.set("key", Json(key));
  body.set("value", Json(value));
  return must_node(node.port, "node.write", std::move(body));
}

Result<Json> EvolutionFixture::try_write_to(const NodeProcess& node, const std::string& key,
                                            const std::string& value) {
  auto claim = claim_of(node);
  Json body = Json::object();
  body.set("claim", to_json(claim));
  body.set("key", Json(key));
  body.set("value", Json(value));
  return node_call(node.port, "node.write", std::move(body));
}

Json EvolutionFixture::read_from(const NodeProcess& node, const std::string& key, bool allow_stale) {
  return must_node(node.port, "node.read",
                   Json::object({{"key", Json(key)}, {"allow_stale", Json(allow_stale)}}));
}

std::string write_compatibility_registry(const std::string& path,
                                                       const std::vector<CompatibilityDecision>& decisions) {
  Json document = Json::object();
  document.set("registry", Json("fabric-compatibility-registry"));
  Json entries = Json::array();
  for (const CompatibilityDecision& decision : decisions) {
    entries.push_back(to_json(decision));
  }
  document.set("decisions", std::move(entries));
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    FABRIC_FAIL("cannot write the compatibility registry file: " + path);
  }
  stream << document.dump();
  stream.close();
  return path;
}

std::string write_manifest_file(const std::string& path, const EvolutionManifest& manifest) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    FABRIC_FAIL("cannot write the manifest file: " + path);
  }
  stream << to_json(manifest).dump();
  stream.close();
  return path;
}

}  // namespace fabric::evolution::test
