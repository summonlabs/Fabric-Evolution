// Fabric Evolution — reference replicated control-plane node.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// This is a real process with a durable state directory, an integrity-checked
// state file, an append-only record log and a TCP server. It is the component
// under evolution in the end-to-end proof.
//
// What is modelled explicitly:
//   * one component identity, one incarnation per boot (a restart is a new
//     incarnation with a strictly greater boot counter);
//   * a versioned key/value state document with a schema version and a log
//     sequence number;
//   * authority leases: a node only mutates while it holds a live, matching
//     lease, and it refuses everything mutating once the lease expires, is
//     fenced, or has been retired;
//   * one-way replication from an authoritative predecessor to a successor
//     (snapshot plus incremental records), where accepted state is bound to the
//     source incarnation, epoch, generation and migration version.
//
// What is deliberately NOT modelled: there is no consensus protocol, no quorum,
// no leader election and no distributed transaction. Authority is granted by the
// controller and enforced by leases with a bounded TTL plus explicit fencing.
// Nothing in this repository claims otherwise.

#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "fabric/evolution/authority.hpp"
#include "fabric/evolution/clock.hpp"
#include "fabric/evolution/epoch_log.hpp"
#include "fabric/evolution/handoff.hpp"
#include "fabric/evolution/migration.hpp"
#include "fabric/evolution/persistence.hpp"
#include "fabric/evolution/protocol.hpp"
#include "fabric/evolution/transport.hpp"

namespace fabric::evolution {

enum class NodeLifecycle : std::uint8_t {
  Unopened = 0,
  Recovering = 1,  // durable state loaded, lease not yet re-confirmed
  Serving = 2,     // accepting requests
  Fenced = 3,      // explicitly fenced by the controller
  Retired = 4,     // permanently out of service; refuses every mutation
  Stopped = 5,
};

[[nodiscard]] std::string_view to_string(NodeLifecycle lifecycle) noexcept;

// A component's own attestation of its epoch, generation and incarnation. The
// controller reconciles its durable view against these reports.
struct NodeReport {
  ComponentId component;
  IncarnationId incarnation;
  ShardId shard;
  SoftwareVersion software;
  ProtocolVersion protocol;
  SchemaVersion schema;
  FeatureSet features;
  EpochNumber epoch;
  Generation generation;
  LogSequenceNumber lsn;
  AuthorityMode mode = AuthorityMode::None;
  AuthorityToken token;
  TimestampMs lease_expires_at_ms = 0;
  bool has_authority = false;
  bool lease_confirmed = false;
  NodeLifecycle lifecycle = NodeLifecycle::Unopened;
  std::uint64_t writes_applied = 0;
  std::uint64_t writes_refused = 0;
  std::uint64_t reads_served = 0;
  Digest state_digest;
  Digest recorded_snapshot_digest;
  LogSequenceNumber oldest_logged_lsn;

  [[nodiscard]] Json to_json() const;
};

// The source binding a successor must honour when it accepts replicated state.
struct SourceBinding {
  IncarnationId source;
  EpochNumber epoch;
  Generation generation;
  AuthorityToken token;
  SchemaVersion schema;
  HandoffId handoff;

  [[nodiscard]] bool matches(const SourceBinding& other) const noexcept {
    return source == other.source && epoch == other.epoch && generation == other.generation &&
           token == other.token && schema == other.schema;
  }
};

struct ReplicationRecord {
  LogSequenceNumber lsn;
  std::string key;
  std::string value;
  EpochNumber epoch;
  Generation generation;
  IncarnationId origin;
  Digest digest;

  [[nodiscard]] Json to_json() const;
  [[nodiscard]] static Result<ReplicationRecord> from_json(const Json& value);
};

// Content digest of a replication record. It binds the record to its log
// position, its key/value payload and the incarnation that produced it.
[[nodiscard]] Digest replication_record_digest(const ReplicationRecord& record);

// Bounded fault-injection surface. It exists so that failure paths (lost
// acknowledgements, corrupt snapshots, protocol mismatch) are exercised by the
// test suite instead of being argued about.
struct FaultRule {
  std::string op;
  std::string action;
  std::uint32_t remaining = 0;
};

struct NodeOptions {
  ComponentId component;
  ShardId shard;
  std::string state_directory;
  std::string host = "127.0.0.1";
  std::uint16_t port = 0;
  SoftwareVersion software = SoftwareVersion::of(1, 0, 0);
  ProtocolVersion protocol = ProtocolVersion::of(1, 0);
  SchemaVersion schema = SchemaVersion::from_value(1);
  FeatureSet features;
  std::size_t worker_threads = 8;
  std::size_t max_queued_connections = 64;
  std::uint64_t io_deadline_ms = 30000;
  TimestampMs lease_ttl_ms = 5000;
  std::size_t max_log_records = 20000;
  std::size_t max_records_per_response = 512;
};

class ControlPlaneNode {
 public:
  ControlPlaneNode(NodeOptions options, Clock& clock);
  ~ControlPlaneNode();

  ControlPlaneNode(const ControlPlaneNode&) = delete;
  ControlPlaneNode& operator=(const ControlPlaneNode&) = delete;

  // Loads durable identity and state, allocates a fresh incarnation and
  // increments the durable boot counter. Refuses to boot on damaged identity.
  [[nodiscard]] Status open();
  [[nodiscard]] Status start_server();
  void stop();

  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] const IncarnationId& incarnation() const noexcept { return incarnation_; }
  [[nodiscard]] const ComponentId& component() const noexcept { return options_.component; }
  [[nodiscard]] NodeReport report() const;
  [[nodiscard]] RpcResponse handle(const RpcRequest& request, const RequestContext& context);

  // Inspection helpers used by tests and the CLI.
  [[nodiscard]] std::optional<std::string> read_state(std::string_view key) const;
  [[nodiscard]] SchemaVersion schema() const;
  [[nodiscard]] LogSequenceNumber lsn() const;
  [[nodiscard]] NodeLifecycle lifecycle() const;
  void set_fault(std::string op, std::string action, std::uint32_t count);

 private:
  [[nodiscard]] Status load_identity();
  [[nodiscard]] Status save_identity();
  [[nodiscard]] Status load_state();
  [[nodiscard]] Status save_state();
  [[nodiscard]] Status load_records();
  [[nodiscard]] Status append_record_entry(const ReplicationRecord& record);
  [[nodiscard]] Status compact_records();
  [[nodiscard]] Status rewrite_records();
  [[nodiscard]] Status apply_write(const std::string& key, const std::string& value,
                                   EpochNumber epoch, Generation generation,
                                   const IncarnationId& origin);
  [[nodiscard]] Status require_authority(const AuthorityClaim& claim, AuthorityMode required,
                                         std::string_view op) const;
  [[nodiscard]] NodeReport report_locked() const;
  [[nodiscard]] std::optional<std::string> take_fault(const std::string& op);
  void set_fault_locked(std::string op, std::string action, std::uint32_t count);
  [[nodiscard]] RpcResponse dispatch(const RpcRequest& request);
  [[nodiscard]] std::string identity_path() const;
  [[nodiscard]] std::string state_path() const;
  [[nodiscard]] std::string records_path() const;

  NodeOptions options_;
  Clock* clock_;
  mutable std::mutex mutex_;
  IncarnationId incarnation_;
  BootCounter boot_counter_;
  NodeLifecycle lifecycle_ = NodeLifecycle::Unopened;

  StateDocument document_;
  LogSequenceNumber lsn_;
  std::uint64_t operation_id_ = 0;
  std::vector<ReplicationRecord> records_;
  MigrationJournal journal_;
  std::optional<AuthorityLease> lease_;
  bool lease_confirmed_ = false;
  bool retired_marker_ = false;

  FenceId last_fence_id_;
  std::optional<SourceBinding> expected_source_;
  Digest recorded_snapshot_digest_;
  SnapshotId last_snapshot_id_;
  LogSequenceNumber last_snapshot_lsn_;
  HandoffId current_handoff_;

  std::vector<FaultRule> faults_;

  std::uint64_t writes_applied_ = 0;
  std::uint64_t writes_refused_ = 0;
  std::uint64_t reads_served_ = 0;

  FileLock directory_lock_;
  std::unique_ptr<FabricServer> server_;
  std::uint16_t port_ = 0;
};

}  // namespace fabric::evolution
