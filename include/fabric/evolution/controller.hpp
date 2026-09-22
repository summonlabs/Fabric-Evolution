// Fabric Evolution — the evolution controller.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The controller owns evolution correctness for one shard: it consumes
// compatibility evidence, validates the manifest, drives the handoff phases over
// real sockets, keeps the authority registry, and persists enough durable state
// that a controller that is killed mid-handoff resumes from the persisted phase
// instead of guessing.
//
// Restart behaviour: durable state is integrity checked on load, every restored
// authority lease is marked unconfirmed, and the controller reconciles its view
// against what each component reports about its own epoch, generation and
// incarnation. A component that reports a state the controller cannot explain
// causes the campaign to stop with a refusal rather than proceed.
//
// The controller performs every phase transition durably before it is
// acknowledged and never grants mutating authority while a fence is outstanding.

#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "fabric/evolution/authority.hpp"
#include "fabric/evolution/campaign.hpp"
#include "fabric/evolution/compatibility.hpp"
#include "fabric/evolution/epoch_log.hpp"
#include "fabric/evolution/explain.hpp"
#include "fabric/evolution/handoff.hpp"
#include "fabric/evolution/manifest.hpp"
#include "fabric/evolution/node.hpp"
#include "fabric/evolution/transport.hpp"

namespace fabric::evolution {

struct EndpointRef {
  ComponentId component;
  std::string host;
  std::uint16_t port = 0;
  std::string role;  // "source" | "target"
};

class NodeClient {
 public:
  NodeClient(EndpointRef endpoint, ServerOptions options);
  [[nodiscard]] Result<Json> call(const std::string& op, Json body) const;
  [[nodiscard]] const EndpointRef& endpoint() const noexcept { return endpoint_; }
  [[nodiscard]] Result<NodeReport> report() const;
  [[nodiscard]] Result<ProtocolHello> hello() const;

 private:
  EndpointRef endpoint_;
  ServerOptions options_;
};

struct ControllerOptions {
  ComponentId controller_id;
  std::string state_directory;
  std::string host = "127.0.0.1";
  std::uint16_t port = 0;
  std::size_t worker_threads = 8;
  std::uint64_t io_deadline_ms = 30000;
  TimestampMs lease_ttl_ms = 6000;
  std::uint32_t max_phase_attempts = 5;
  std::size_t max_catch_up_rounds = 128;
  std::size_t max_records_per_round = 256;
  EvidencePolicy evidence_policy = EvidencePolicy::RequireExactMatch;
};

class EvolutionController {
 public:
  EvolutionController(ControllerOptions options, CompatibilityRegistry& registry, Clock& clock);
  ~EvolutionController();

  EvolutionController(const EvolutionController&) = delete;
  EvolutionController& operator=(const EvolutionController&) = delete;

  [[nodiscard]] Status open();
  [[nodiscard]] Status start_admin();
  void stop();

  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

  // Lifecycle operations. Each returns the updated status document, and each
  // fills an Explanation that states inputs, evidence, policy and the rejected
  // alternatives.
  [[nodiscard]] Result<Json> plan(const EvolutionManifest& manifest, Explanation& explanation);
  [[nodiscard]] Result<Json> preflight(Explanation& explanation);
  [[nodiscard]] Result<Json> start(Explanation& explanation);
  [[nodiscard]] Result<Json> advance(Explanation& explanation);
  [[nodiscard]] Result<Json> run_to_completion(Explanation& explanation);
  [[nodiscard]] Result<Json> pause(Explanation& explanation);
  [[nodiscard]] Result<Json> resume(Explanation& explanation);
  [[nodiscard]] Result<Json> abort(Explanation& explanation);
  [[nodiscard]] Result<Json> reconcile(Explanation& explanation);

  [[nodiscard]] Json status_json() const;
  [[nodiscard]] Json authority_json() const;
  [[nodiscard]] Json epoch_json() const;
  [[nodiscard]] Json handoff_json() const;
  [[nodiscard]] Result<Json> explain_topic(const std::string& topic) const;
  [[nodiscard]] Result<Json> admin(const RpcRequest& request);

  // Test and inspection hooks.
  [[nodiscard]] const AuthorityRegistry& authority() const noexcept { return authority_; }
  [[nodiscard]] std::optional<CampaignRecord> campaign() const;
  [[nodiscard]] std::optional<HandoffRecord> handoff() const;
  [[nodiscard]] std::optional<EvolutionManifest> manifest() const;
  // Used by tests to install a manifest without going through the wire.
  void set_manifest_for_test(EvolutionManifest manifest);

 private:
  [[nodiscard]] std::string state_path() const;
  [[nodiscard]] Status load_state();
  [[nodiscard]] Status save_state();

  // Implementations. Every one of these requires the controller lock to be held
  // by the caller and never acquires it, so no public entry point can re-enter
  // the mutex.
  // Turns a completed step into the controller status document. Failures
  // propagate unchanged; success means the caller's step is done.
  [[nodiscard]] Result<Json> completed(const Status& status) const;
  [[nodiscard]] Result<Json> plan_locked(const EvolutionManifest& manifest, Explanation& explanation);
  [[nodiscard]] Result<Json> preflight_locked(Explanation& explanation);
  [[nodiscard]] Status begin_handoff_locked(Explanation& explanation);
  [[nodiscard]] Result<Json> start_locked(Explanation& explanation);
  [[nodiscard]] Result<Json> advance_locked(Explanation& explanation);
  [[nodiscard]] Result<Json> run_to_completion_locked(Explanation& explanation);
  [[nodiscard]] Result<Json> pause_locked(Explanation& explanation);
  [[nodiscard]] Result<Json> resume_locked(Explanation& explanation);
  [[nodiscard]] Result<Json> abort_locked(Explanation& explanation);
  [[nodiscard]] Result<Json> reconcile_locked(Explanation& explanation);
  [[nodiscard]] Json status_json_locked() const;
  [[nodiscard]] Json authority_json_locked() const;
  [[nodiscard]] Json epoch_json_locked() const;
  [[nodiscard]] Json handoff_json_locked() const;
  [[nodiscard]] Result<Json> explain_topic_locked(const std::string& topic) const;
  [[nodiscard]] WindowStatus window_status_locked() const;

  [[nodiscard]] Status apply_action(CampaignAction action, Explanation& explanation);
  [[nodiscard]] Status ensure_predecessor_authority(Explanation& explanation);
  [[nodiscard]] Status ensure_successor_authority(Explanation& explanation);
  [[nodiscard]] Status ensure_holder_authority(const EndpointRef& endpoint, const NodeReport& report,
                                               Explanation& explanation);
  // Re-binds the handoff to the incarnations that are actually running and
  // revokes leases held by incarnations that a fresh boot has provably replaced.
  [[nodiscard]] Status reconcile_incarnations(Explanation& explanation);
  [[nodiscard]] Status restart_handoff(const IncarnationId& predecessor,
                                       const IncarnationId& successor, HandoffPhase rewind_to,
                                       std::string note);
  [[nodiscard]] std::optional<AuthorityClaim> current_source_claim() const;
  [[nodiscard]] static bool incarnation_provably_replaced(const IncarnationId& recorded,
                                                          const IncarnationId& reported);
  [[nodiscard]] Result<Json> advance_prepared(Explanation& explanation);
  [[nodiscard]] Result<Json> advance_snapshot(Explanation& explanation);
  [[nodiscard]] Result<Json> advance_catch_up(Explanation& explanation);
  [[nodiscard]] Result<Json> advance_readiness(Explanation& explanation);
  [[nodiscard]] Result<Json> advance_fence(Explanation& explanation);
  [[nodiscard]] Result<Json> advance_transfer(Explanation& explanation);
  [[nodiscard]] Result<Json> advance_verify(Explanation& explanation);
  [[nodiscard]] Result<Json> advance_retire(Explanation& explanation);
  [[nodiscard]] Status record_phase(HandoffPhase to, std::string note);
  [[nodiscard]] Status bind_nodes();
  [[nodiscard]] Digest current_manifest_digest() const;
  [[nodiscard]] DecisionAuthority decision_authority() const;

  ControllerOptions options_;
  CompatibilityRegistry* registry_;
  Clock* clock_;
  mutable std::mutex mutex_;
  AuthorityRegistry authority_;
  EpochLog epoch_log_;

  std::optional<EvolutionManifest> manifest_;
  std::optional<CampaignRecord> campaign_;
  std::optional<HandoffRecord> handoff_;
  std::optional<EndpointRef> source_endpoint_;
  std::optional<EndpointRef> target_endpoint_;
  std::optional<NodeReport> last_source_report_;
  std::optional<NodeReport> last_target_report_;
  std::vector<IncarnationId> predecessor_history_;
  std::optional<AuthorityClaim> fenced_predecessor_claim_;
  LogSequenceNumber window_start_lsn_;
  std::uint64_t next_handoff_id_ = 1;
  EpochNumber next_epoch_ = EpochNumber::from_value(1);
  IncarnationId controller_incarnation_;
  BootCounter controller_boot_ = BootCounter::from_value(1);
  TimestampMs last_pause_deferral_ms_ = 0;
  std::string last_deferral_reason_;
  bool recovered_from_backup_ = false;

  std::unique_ptr<FabricServer> server_;
  std::uint16_t port_ = 0;
};

// Loads a manifest from a file, reporting IO and parse failures distinctly.
[[nodiscard]] Result<EvolutionManifest> load_manifest_file(const std::string& path);

}  // namespace fabric::evolution
