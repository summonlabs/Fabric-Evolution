// Fabric Evolution — reference replicated control-plane node (implementation).
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric/evolution/node.hpp"

#include <algorithm>
#include <random>

namespace fabric::evolution {
namespace {

[[nodiscard]] Uuid128 random_uuid() {
  static std::mutex random_mutex;
  static std::mt19937_64 generator{std::random_device{}()};
  std::uint64_t words[2] = {0, 0};
  {
    std::lock_guard<std::mutex> lock(random_mutex);
    words[0] = generator();
    words[1] = generator();
  }
  Uuid128::bytes_type bytes{};
  for (int index = 0; index < 8; ++index) {
    bytes[static_cast<std::size_t>(index)] =
        static_cast<std::uint8_t>((words[0] >> (8U * static_cast<unsigned>(index))) & 0xFFU);
    bytes[static_cast<std::size_t>(index) + 8] =
        static_cast<std::uint8_t>((words[1] >> (8U * static_cast<unsigned>(index))) & 0xFFU);
  }
  if (Uuid128(bytes).is_nil()) {
    bytes[15] = 1;
  }
  return Uuid128(bytes);
}

[[nodiscard]] Digest snapshot_digest(const StateDocument& document, LogSequenceNumber lsn) {
  Json out = Json::object();
  out.set("schema", Json(document.schema().value()));
  out.set("lsn", Json(lsn.value()));
  Json fields = Json::object();
  for (const auto& entry : document.fields()) {
    fields.set(entry.first, Json(entry.second));
  }
  out.set("fields", std::move(fields));
  return sha256(out.dump());
}

[[nodiscard]] Status parse_handoff_id(const Json& body, HandoffId& out) {
  const auto value = json_u64(body, "handoff");
  if (!value.has_value() || *value == 0) {
    return invalid_argument("request is missing a positive handoff id");
  }
  out = HandoffId::from_value(*value);
  return Status::success();
}

[[nodiscard]] Result<SourceBinding> parse_source_binding(const Json& value) {
  if (!value.is_object()) {
    return malformed("source binding must be an object");
  }
  SourceBinding binding;
  const Json* incarnation = value.find("incarnation");
  if (incarnation == nullptr) {
    return malformed("source binding is missing incarnation");
  }
  auto parsed_incarnation = incarnation_from_json(*incarnation);
  if (!parsed_incarnation.ok()) {
    return parsed_incarnation.status();
  }
  binding.source = parsed_incarnation.value();
  const auto epoch = json_u64(value, "epoch");
  const auto generation = json_u64(value, "generation");
  if (!epoch.has_value() || *epoch == 0 || !generation.has_value() || *generation == 0) {
    return malformed("source binding epoch/generation must be positive");
  }
  binding.epoch = EpochNumber::from_value(*epoch);
  binding.generation = Generation::from_value(*generation);
  const auto token = json_string(value, "token");
  if (!token.has_value()) {
    return malformed("source binding is missing token");
  }
  auto parsed_token = AuthorityToken::parse(*token);
  if (!parsed_token.has_value() || parsed_token->is_nil()) {
    return malformed("source binding token is not a valid uuid");
  }
  binding.token = *parsed_token;
  const auto schema = json_u64(value, "schema");
  if (!schema.has_value() || *schema == 0 || *schema > UINT32_MAX) {
    return malformed("source binding schema is missing or out of range");
  }
  binding.schema = SchemaVersion::from_value(static_cast<std::uint32_t>(*schema));
  binding.handoff = HandoffId::from_value(json_u64_or(value, "handoff", 0));
  return binding;
}

[[nodiscard]] Json source_binding_to_json(const SourceBinding& binding) {
  Json out = Json::object();
  out.set("incarnation", ::fabric::evolution::to_json(binding.source));
  out.set("epoch", Json(binding.epoch.value()));
  out.set("generation", Json(binding.generation.value()));
  out.set("token", Json(binding.token.to_compact_string()));
  out.set("schema", Json(binding.schema.value()));
  out.set("handoff", Json(binding.handoff.value()));
  return out;
}

[[nodiscard]] Result<FeatureSet> parse_features(const Json& value, const char* field) {
  FeatureSet set;
  if (value.is_null()) {
    return set;
  }
  if (!value.is_array()) {
    return malformed(std::string(field) + " must be an array of feature names");
  }
  std::vector<std::string> names;
  for (std::size_t index = 0; index < value.size(); ++index) {
    const std::string* name = value.at(index).try_string();
    if (name == nullptr) {
      return malformed(std::string(field) + " entries must be strings");
    }
    names.push_back(*name);
  }
  std::string error;
  const auto parsed = FeatureSet::from_names(names, error);
  if (!parsed.has_value()) {
    return malformed(error);
  }
  return *parsed;
}

[[nodiscard]] std::string_view lifecycle_name(NodeLifecycle lifecycle) noexcept {
  switch (lifecycle) {
    case NodeLifecycle::Unopened:
      return "unopened";
    case NodeLifecycle::Recovering:
      return "recovering";
    case NodeLifecycle::Serving:
      return "serving";
    case NodeLifecycle::Fenced:
      return "fenced";
    case NodeLifecycle::Retired:
      return "retired";
    case NodeLifecycle::Stopped:
      return "stopped";
  }
  return "unknown";
}

}  // namespace

std::string_view to_string(NodeLifecycle lifecycle) noexcept { return lifecycle_name(lifecycle); }

Json NodeReport::to_json() const {
  Json out = Json::object();
  out.set("component", Json(component.str()));
  out.set("incarnation", ::fabric::evolution::to_json(incarnation));
  out.set("shard", Json(shard.str()));
  out.set("software", Json(software.to_string()));
  out.set("protocol", Json(protocol.to_string()));
  out.set("schema", Json(schema.value()));
  Json feature_array = Json::array();
  for (const std::string& name : features.names()) {
    feature_array.push_back(Json(name));
  }
  out.set("features", std::move(feature_array));
  out.set("epoch", Json(epoch.value()));
  out.set("generation", Json(generation.value()));
  out.set("lsn", Json(lsn.value()));
  out.set("mode", Json(std::string(::fabric::evolution::to_string(mode))));
  out.set("token", Json(token.to_compact_string()));
  out.set("lease_expires_at_ms", Json(lease_expires_at_ms));
  out.set("has_authority", Json(has_authority));
  out.set("lease_confirmed", Json(lease_confirmed));
  out.set("lifecycle", Json(std::string(lifecycle_name(lifecycle))));
  out.set("writes_applied", Json(writes_applied));
  out.set("writes_refused", Json(writes_refused));
  out.set("reads_served", Json(reads_served));
  out.set("state_digest", Json(state_digest.to_hex()));
  out.set("recorded_snapshot_digest", Json(recorded_snapshot_digest.to_hex()));
  out.set("oldest_logged_lsn", Json(oldest_logged_lsn.value()));
  return out;
}

Json ReplicationRecord::to_json() const {
  Json out = Json::object();
  out.set("lsn", Json(lsn.value()));
  out.set("key", Json(key));
  out.set("value", Json(value));
  out.set("epoch", Json(epoch.value()));
  out.set("generation", Json(generation.value()));
  out.set("origin", ::fabric::evolution::to_json(origin));
  out.set("digest", Json(digest.to_hex()));
  return out;
}

Digest replication_record_digest(const ReplicationRecord& record) {
  Json out = Json::object();
  out.set("lsn", Json(record.lsn.value()));
  out.set("key", Json(record.key));
  out.set("value", Json(record.value));
  out.set("epoch", Json(record.epoch.value()));
  out.set("generation", Json(record.generation.value()));
  out.set("origin", ::fabric::evolution::to_json(record.origin));
  return sha256(out.dump());
}

Result<ReplicationRecord> ReplicationRecord::from_json(const Json& value) {
  if (!value.is_object()) {
    return malformed("replication record must be an object");
  }
  ReplicationRecord record;
  const auto lsn = json_u64(value, "lsn");
  if (!lsn.has_value() || *lsn == 0) {
    return malformed("replication record lsn must be positive");
  }
  record.lsn = LogSequenceNumber::from_value(*lsn);
  const auto key = json_string(value, "key");
  if (!key.has_value() || key->empty() || key->size() > kMaxStateKeyBytes) {
    return malformed("replication record key is missing or out of range");
  }
  record.key = *key;
  const auto entry_value = json_string(value, "value");
  if (!entry_value.has_value() || entry_value->size() > kMaxStateValueBytes) {
    return malformed("replication record value is missing or out of range");
  }
  record.value = *entry_value;
  const auto epoch = json_u64(value, "epoch");
  const auto generation = json_u64(value, "generation");
  if (!epoch.has_value() || *epoch == 0 || !generation.has_value() || *generation == 0) {
    return malformed("replication record epoch/generation must be positive");
  }
  record.epoch = EpochNumber::from_value(*epoch);
  record.generation = Generation::from_value(*generation);
  const Json* origin = value.find("origin");
  if (origin == nullptr) {
    return malformed("replication record is missing origin");
  }
  auto parsed_origin = incarnation_from_json(*origin);
  if (!parsed_origin.ok()) {
    return parsed_origin.status();
  }
  record.origin = parsed_origin.value();
  record.digest = replication_record_digest(record);
  if (const auto recorded = json_string(value, "digest")) {
    auto parsed = Digest::from_hex(*recorded);
    if (!parsed.has_value()) {
      return malformed("replication record digest is not a sha256 hex digest");
    }
    if (*parsed != record.digest) {
      return Status::error(ErrorCode::IntegrityFailure,
                           "replication record digest does not match its contents");
    }
  }
  return record;
}

ControlPlaneNode::ControlPlaneNode(NodeOptions options, Clock& clock)
    : options_(std::move(options)), clock_(&clock), lsn_(LogSequenceNumber::from_value(0)),
      journal_(1024) {}

ControlPlaneNode::~ControlPlaneNode() { stop(); }

std::string ControlPlaneNode::identity_path() const { return options_.state_directory + "/identity.fxi"; }
std::string ControlPlaneNode::state_path() const { return options_.state_directory + "/state.fxi"; }
std::string ControlPlaneNode::records_path() const { return options_.state_directory + "/records.fxr"; }

Status ControlPlaneNode::load_identity() {
  if (!path_exists(identity_path())) {
    boot_counter_ = BootCounter::from_value(1);
  } else {
    auto raw = read_integrity_file(identity_path(), nullptr);
    if (!raw.ok()) {
      return Status::error(raw.status().code(),
                           "durable node identity is damaged; refusing to boot with a guessed identity: " +
                               raw.status().message(),
                           raw.status().detail());
    }
    const JsonParseResult parsed = parse_json(raw.value());
    if (!parsed.ok()) {
      return Status::error(ErrorCode::IntegrityFailure,
                           "durable node identity is not valid JSON: " + parsed.error);
    }
    const auto component = json_string(*parsed.value, "component");
    if (!component.has_value() || *component != options_.component.str()) {
      return Status::error(ErrorCode::IntegrityFailure,
                           "durable node identity belongs to a different component",
                           Json::object({{"recorded", Json(component.has_value() ? *component : std::string())},
                                         {"configured", Json(options_.component.str())}}));
    }
    const auto previous = json_u64(*parsed.value, "boot_counter");
    if (!previous.has_value()) {
      return Status::error(ErrorCode::IntegrityFailure,
                           "durable node identity has no boot counter");
    }
    const auto next = BootCounter::from_value(*previous).next();
    if (!next.has_value()) {
      return Status::error(ErrorCode::BoundsExceeded, "node boot counter is exhausted");
    }
    boot_counter_ = *next;
  }
  incarnation_ = IncarnationId(options_.component, IncarnationUuid::from_value(random_uuid()), boot_counter_);
  return save_identity();
}

Status ControlPlaneNode::save_identity() {
  Json out = Json::object();
  out.set("component", Json(options_.component.str()));
  out.set("boot_counter", Json(boot_counter_.value()));
  out.set("incarnation", ::fabric::evolution::to_json(incarnation_));
  out.set("software", Json(options_.software.to_string()));
  return write_integrity_file(identity_path(), out.dump(), false);
}

Status ControlPlaneNode::load_state() {
  if (!path_exists(state_path())) {
    document_ = StateDocument(options_.schema);
    lsn_ = LogSequenceNumber::from_value(0);
    return Status::success();
  }
  auto recovered = read_integrity_file_with_recovery(state_path(), true);
  if (!recovered.ok()) {
    return Status::error(recovered.status().code(),
                         "durable node state is damaged and cannot be recovered: " +
                             recovered.status().message(),
                         recovered.status().detail());
  }
  const JsonParseResult parsed = parse_json(recovered.value().payload);
  if (!parsed.ok()) {
    return Status::error(ErrorCode::IntegrityFailure,
                         "durable node state is not valid JSON: " + parsed.error);
  }
  const Json& document = *parsed.value;
  const Json* fields = document.find("fields");
  if (fields == nullptr || !fields->is_object()) {
    return Status::error(ErrorCode::IntegrityFailure, "durable node state has no fields object");
  }
  const auto schema = json_u64(document, "schema");
  if (!schema.has_value() || *schema == 0 || *schema > UINT32_MAX) {
    return Status::error(ErrorCode::IntegrityFailure, "durable node state has no valid schema version");
  }
  StateDocument restored(SchemaVersion::from_value(static_cast<std::uint32_t>(*schema)));
  for (const auto& entry : *fields->try_object()) {
    const std::string* text = entry.second.try_string();
    if (text == nullptr) {
      return Status::error(ErrorCode::IntegrityFailure,
                           "durable node state field is not a string: " + entry.first);
    }
    const Status put = restored.put(entry.first, *text);
    if (!put.ok()) {
      return put;
    }
  }
  document_ = std::move(restored);
  lsn_ = LogSequenceNumber::from_value(json_u64_or(document, "lsn", 0));
  operation_id_ = json_u64_or(document, "operation_id", 0);
  retired_marker_ = json_bool_or(document, "retired", false);
  if (const Json* journal = document.find("migration_journal")) {
    const Status loaded = journal_.load(*journal);
    if (!loaded.ok()) {
      return loaded;
    }
  }
  if (const Json* lease = document.find("lease")) {
    auto parsed_lease = authority_lease_from_json(*lease);
    if (parsed_lease.ok()) {
      lease_ = parsed_lease.take();
      // A lease restored from disk is evidence, not authority: it stays
      // unconfirmed until the controller re-establishes it for this incarnation.
      lease_->attested = false;
      lease_confirmed_ = false;
    }
  }
  last_snapshot_lsn_ = LogSequenceNumber::from_value(json_u64_or(document, "snapshot_lsn", 0));
  last_snapshot_id_ = SnapshotId::from_value(json_u64_or(document, "snapshot_id", 0));
  if (const auto digest = json_string(document, "snapshot_digest")) {
    if (auto parsed_digest = Digest::from_hex(*digest)) {
      recorded_snapshot_digest_ = *parsed_digest;
    }
  }
  return Status::success();
}

Status ControlPlaneNode::save_state() {
  Json out = Json::object();
  out.set("schema", Json(document_.schema().value()));
  Json fields = Json::object();
  for (const auto& entry : document_.fields()) {
    fields.set(entry.first, Json(entry.second));
  }
  out.set("fields", std::move(fields));
  out.set("lsn", Json(lsn_.value()));
  out.set("operation_id", Json(operation_id_));
  out.set("retired", Json(retired_marker_));
  out.set("migration_journal", journal_.to_json());
  out.set("snapshot_lsn", Json(last_snapshot_lsn_.value()));
  out.set("snapshot_id", Json(last_snapshot_id_.value()));
  out.set("snapshot_digest", Json(recorded_snapshot_digest_.to_hex()));
  if (lease_.has_value()) {
    out.set("lease", ::fabric::evolution::to_json(*lease_));
  }
  return write_integrity_file(state_path(), out.dump(), true);
}

Status ControlPlaneNode::load_records() {
  records_.clear();
  auto report = read_records(records_path());
  if (!report.ok()) {
    return report.status();
  }
  bool dropped_uncommitted = false;
  for (const std::string& record : report.value().records) {
    const JsonParseResult parsed = parse_json(record);
    if (!parsed.ok()) {
      return Status::error(ErrorCode::IntegrityFailure,
                           "record log entry is not valid JSON: " + parsed.error);
    }
    auto entry = ReplicationRecord::from_json(*parsed.value);
    if (!entry.ok()) {
      return entry.status();
    }
    // The durable state file is the commit point. A record beyond the committed
    // log position was never acknowledged, so it is dropped rather than replayed
    // as if it had been accepted.
    if (entry.value().lsn.value() > lsn_.value()) {
      dropped_uncommitted = true;
      continue;
    }
    records_.push_back(entry.take());
  }
  if (dropped_uncommitted) {
    return rewrite_records();
  }
  return Status::success();
}

Status ControlPlaneNode::rewrite_records() {
  std::string payload;
  for (const ReplicationRecord& record : records_) {
    const std::string encoded = record.to_json().dump();
    std::string header;
    header.append("FXR1", 4);
    append_u32_le(header, 1U);
    append_u64_le(header, static_cast<std::uint64_t>(encoded.size()));
    append_u32_le(header, crc32(encoded));
    append_u32_le(header, 0U);
    payload += header;
    payload += encoded;
  }
  return write_bytes_atomic(records_path(), payload);
}

Status ControlPlaneNode::append_record_entry(const ReplicationRecord& record) {
  const Status appended = append_record(records_path(), record.to_json().dump());
  if (!appended.ok()) {
    return appended;
  }
  records_.push_back(record);
  if (records_.size() > options_.max_log_records) {
    return compact_records();
  }
  return Status::success();
}

Status ControlPlaneNode::compact_records() {
  const std::size_t keep = options_.max_log_records / 2 == 0 ? 1 : options_.max_log_records / 2;
  std::vector<ReplicationRecord> retained;
  const std::size_t start = records_.size() > keep ? records_.size() - keep : 0;
  std::string payload;
  for (std::size_t index = start; index < records_.size(); ++index) {
    retained.push_back(records_[index]);
    const std::string encoded = records_[index].to_json().dump();
    std::string header;
    header.append("FXR1", 4);
    append_u32_le(header, 1U);
    append_u64_le(header, static_cast<std::uint64_t>(encoded.size()));
    append_u32_le(header, crc32(encoded));
    append_u32_le(header, 0U);
    payload += header;
    payload += encoded;
  }
  const Status written = write_bytes_atomic(records_path(), payload);
  if (!written.ok()) {
    return written;
  }
  records_ = std::move(retained);
  return Status::success();
}

Status ControlPlaneNode::apply_write(const std::string& key, const std::string& value, EpochNumber epoch,
                                     Generation generation, const IncarnationId& origin) {
  const auto next = lsn_.next();
  if (!next.has_value()) {
    return Status::error(ErrorCode::BoundsExceeded, "log sequence number space is exhausted");
  }
  const Status put = document_.put(key, value);
  if (!put.ok()) {
    return put;
  }
  lsn_ = *next;
  operation_id_ += 1;
  ReplicationRecord record;
  record.lsn = lsn_;
  record.key = key;
  record.value = value;
  record.epoch = epoch;
  record.generation = generation;
  record.origin = origin;
  record.digest = replication_record_digest(record);
  const Status appended = append_record_entry(record);
  if (!appended.ok()) {
    return appended;
  }
  return save_state();
}

Status ControlPlaneNode::require_authority(const AuthorityClaim& claim, AuthorityMode required,
                                           std::string_view op) const {
  if (lifecycle_ == NodeLifecycle::Retired) {
    return Status::error(ErrorCode::NotAuthoritative, "node is retired and refuses every operation",
                         Json::object({{"op", Json(std::string(op))}}));
  }
  if (lifecycle_ == NodeLifecycle::Fenced) {
    return Status::error(ErrorCode::StaleAuthority, "node has been fenced",
                         Json::object({{"op", Json(std::string(op))}}));
  }
  if (!lease_.has_value()) {
    return Status::error(ErrorCode::NotAuthoritative, "node holds no authority lease",
                         Json::object({{"op", Json(std::string(op))}}));
  }
  if (!lease_confirmed_) {
    return Status::error(ErrorCode::StaleAuthority,
                         "authority lease has not been confirmed since this incarnation started",
                         Json::object({{"op", Json(std::string(op))}}));
  }
  const TimestampMs now_ms = clock_->now_ms();
  if (now_ms >= lease_->expires_at_ms) {
    return Status::error(ErrorCode::StaleAuthority, "authority lease has expired",
                         Json::object({{"expired_at_ms", Json(lease_->expires_at_ms)},
                                       {"now_ms", Json(now_ms)}}));
  }
  if (!claim.is_valid()) {
    return invalid_argument("authority claim is incomplete");
  }
  if (claim.incarnation != incarnation_) {
    return Status::error(ErrorCode::StaleIncarnation,
                         "authority claim was issued to a different incarnation",
                         Json::object({{"claimed", Json(claim.incarnation.to_string())},
                                       {"current", Json(incarnation_.to_string())}}));
  }
  if (claim.epoch != lease_->epoch) {
    return Status::error(ErrorCode::StaleEpoch, "authority claim carries a stale epoch",
                         Json::object({{"claimed", Json(claim.epoch.value())},
                                       {"current", Json(lease_->epoch.value())}}));
  }
  if (claim.generation != lease_->generation) {
    return Status::error(ErrorCode::StaleGeneration, "authority claim carries a stale generation",
                         Json::object({{"claimed", Json(claim.generation.value())},
                                       {"current", Json(lease_->generation.value())}}));
  }
  if (claim.token != lease_->token) {
    return Status::error(ErrorCode::StaleAuthority, "authority claim token does not match the lease");
  }
  if (authority_allows_mutation(required) && !authority_allows_mutation(lease_->mode)) {
    return Status::error(ErrorCode::NotAuthoritative, "lease does not permit mutation");
  }
  if (authority_allows_read(required) && !authority_allows_read(lease_->mode)) {
    return Status::error(ErrorCode::NotAuthoritative, "lease does not permit reads");
  }
  return Status::success();
}

std::optional<std::string> ControlPlaneNode::take_fault(const std::string& op) {
  for (FaultRule& rule : faults_) {
    if (rule.op == op && rule.remaining > 0) {
      rule.remaining -= 1;
      return rule.action;
    }
  }
  return std::nullopt;
}

void ControlPlaneNode::set_fault(std::string op, std::string action, std::uint32_t count) {
  std::lock_guard<std::mutex> lock(mutex_);
  set_fault_locked(std::move(op), std::move(action), count);
}

void ControlPlaneNode::set_fault_locked(std::string op, std::string action, std::uint32_t count) {
  for (FaultRule& rule : faults_) {
    if (rule.op == op && rule.action == action) {
      rule.remaining = count;
      return;
    }
  }
  if (faults_.size() >= 16) {
    faults_.erase(faults_.begin());
  }
  faults_.push_back(FaultRule{std::move(op), std::move(action), count});
}

Status ControlPlaneNode::open() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (lifecycle_ != NodeLifecycle::Unopened) {
    return Status::error(ErrorCode::AlreadyExists, "node has already been opened");
  }
  if (!options_.component.is_valid() || !options_.shard.is_valid()) {
    return invalid_argument("node component and shard identities must be valid");
  }
  if (options_.state_directory.empty()) {
    return invalid_argument("node state directory must be set");
  }
  const Status directory = ensure_directory(options_.state_directory);
  if (!directory.ok()) {
    return directory;
  }
  auto directory_lock = FileLock::acquire(options_.state_directory + "/node.lock");
  if (!directory_lock.ok()) {
    return Status::error(directory_lock.status().code(),
                         "cannot take exclusive ownership of the node state directory: " +
                             directory_lock.status().message(),
                         directory_lock.status().detail());
  }
  directory_lock_ = directory_lock.take();
  const Status identity = load_identity();
  if (!identity.ok()) {
    return identity;
  }
  const Status state = load_state();
  if (!state.ok()) {
    return state;
  }
  const Status records = load_records();
  if (!records.ok()) {
    return records;
  }
  lifecycle_ = retired_marker_ ? NodeLifecycle::Retired : NodeLifecycle::Recovering;
  lease_confirmed_ = false;
  // A booted node always has durable state on disk, even before its first write,
  // so recovery never has to guess whether the directory is fresh.
  return save_state();
}

Status ControlPlaneNode::start_server() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (lifecycle_ == NodeLifecycle::Unopened) {
      return Status::error(ErrorCode::IllegalTransition, "node must be opened before serving");
    }
    if (server_ != nullptr) {
      return Status::error(ErrorCode::AlreadyExists, "node server is already running");
    }
  }
  ServerOptions server_options;
  server_options.host = options_.host;
  server_options.port = options_.port;
  server_options.worker_threads = options_.worker_threads;
  server_options.max_queued_connections = options_.max_queued_connections;
  server_options.io_deadline_ms = options_.io_deadline_ms;
  auto server = std::make_unique<FabricServer>(
      server_options, [this](const RpcRequest& request, const RequestContext& context) {
        return handle(request, context);
      });
  const Status started = server->start();
  if (!started.ok()) {
    return started;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  port_ = server->port();
  server_ = std::move(server);
  if (lifecycle_ == NodeLifecycle::Recovering) {
    lifecycle_ = NodeLifecycle::Serving;
  }
  return Status::success();
}

void ControlPlaneNode::stop() {
  std::unique_ptr<FabricServer> server;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    server = std::move(server_);
    port_ = 0;
    if (lifecycle_ != NodeLifecycle::Unopened && lifecycle_ != NodeLifecycle::Retired) {
      lifecycle_ = NodeLifecycle::Stopped;
    }
  }
  if (server != nullptr) {
    server->stop();
  }
}

NodeReport ControlPlaneNode::report_locked() const {
  NodeReport out;
  out.component = options_.component;
  out.incarnation = incarnation_;
  out.shard = options_.shard;
  out.software = options_.software;
  out.protocol = options_.protocol;
  out.schema = document_.schema();
  out.features = options_.features;
  out.epoch = lease_.has_value() ? lease_->epoch : EpochNumber::invalid();
  out.generation = lease_.has_value() ? lease_->generation : Generation::invalid();
  out.lsn = lsn_;
  out.mode = lease_.has_value() ? lease_->mode : AuthorityMode::None;
  out.token = lease_.has_value() ? lease_->token : AuthorityToken::nil();
  out.lease_expires_at_ms = lease_.has_value() ? lease_->expires_at_ms : 0;
  out.has_authority = lease_.has_value() && lease_confirmed_ &&
                      clock_->now_ms() < (lease_.has_value() ? lease_->expires_at_ms : 0);
  out.lease_confirmed = lease_confirmed_;
  out.lifecycle = lifecycle_;
  out.writes_applied = writes_applied_;
  out.writes_refused = writes_refused_;
  out.reads_served = reads_served_;
  out.state_digest = snapshot_digest(document_, lsn_);
  out.recorded_snapshot_digest = recorded_snapshot_digest_;
  out.oldest_logged_lsn = records_.empty() ? lsn_ : records_.front().lsn;
  return out;
}

std::optional<std::string> ControlPlaneNode::read_state(std::string_view key) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return document_.get(key);
}

SchemaVersion ControlPlaneNode::schema() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return document_.schema();
}

LogSequenceNumber ControlPlaneNode::lsn() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return lsn_;
}

NodeLifecycle ControlPlaneNode::lifecycle() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return lifecycle_;
}

NodeReport ControlPlaneNode::report() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return report_locked();
}

RpcResponse ControlPlaneNode::handle(const RpcRequest& request, const RequestContext&) {
  std::lock_guard<std::mutex> lock(mutex_);
  RpcResponse response;
  response.id = request.id;
  response.op = request.op;

  const std::optional<std::string> fault = take_fault(request.op);
  if (fault.has_value() && *fault == "error") {
    response.status = Status::error(ErrorCode::Internal, "injected failure for " + request.op);
    return response;
  }

  response = dispatch(request);
  if (fault.has_value() && *fault == "drop_response") {
    response.suppress_response = true;
  }
  return response;
}

RpcResponse ControlPlaneNode::dispatch(const RpcRequest& request) {
  RpcResponse response;
  response.id = request.id;
  response.op = request.op;
  const Json& body = request.body;

  if (request.op == "node.hello") {
    ProtocolHello hello;
    hello.component = options_.component;
    hello.incarnation = incarnation_;
    hello.software = options_.software;
    hello.protocol = options_.protocol;
    hello.schema = document_.schema();
    hello.features = options_.features;
    hello.epoch = lease_.has_value() ? lease_->epoch : EpochNumber::invalid();
    hello.generation = lease_.has_value() ? lease_->generation : Generation::invalid();
    hello.holds_authority = lease_confirmed_ && lease_.has_value() &&
                            clock_->now_ms() < lease_->expires_at_ms;
    response.body = Json::object({{"hello", ::fabric::evolution::to_json(hello)}});
    return response;
  }

  if (request.op == "node.status") {
    response.body = Json::object({{"report", report_locked().to_json()}});
    return response;
  }

  if (request.op == "node.faults") {
    const auto op = json_string(body, "op");
    const auto action = json_string(body, "action");
    const auto count = json_u64(body, "count");
    if (!op.has_value() || !action.has_value() || !count.has_value() || *count > 1000) {
      response.status = invalid_argument("fault injection requires op, action and count");
      return response;
    }
    set_fault_locked(*op, *action, static_cast<std::uint32_t>(*count));
    return response;
  }

  if (request.op == "node.prepare") {
    HandoffId handoff;
    const Status parsed_handoff = parse_handoff_id(body, handoff);
    if (!parsed_handoff.ok()) {
      response.status = parsed_handoff;
      return response;
    }
    const auto role = json_string(body, "role");
    if (!role.has_value() || (*role != "successor" && *role != "predecessor")) {
      response.status = invalid_argument("prepare requires role successor or predecessor");
      return response;
    }
    const auto manifest_digest = json_string(body, "manifest_digest");
    if (manifest_digest.has_value()) {
      auto parsed = Digest::from_hex(*manifest_digest);
      if (!parsed.has_value()) {
        response.status = malformed("prepare manifest_digest is not a sha256 hex digest");
        return response;
      }
    }
    if (*role == "successor") {
      const auto target = json_string(body, "target_software");
      const auto target_protocol = json_string(body, "target_protocol");
      const auto target_schema = json_u64(body, "target_schema");
      if (!target.has_value() || !target_protocol.has_value() || !target_schema.has_value()) {
        response.status = invalid_argument("successor prepare requires the manifest target spec");
        return response;
      }
      auto parsed_software = SoftwareVersion::parse(*target);
      auto parsed_protocol = ProtocolVersion::parse(*target_protocol);
      if (!parsed_software.has_value() || !parsed_protocol.has_value()) {
        response.status = malformed("successor prepare target spec is invalid");
        return response;
      }
      if (*parsed_software != options_.software) {
        response.status = Status::error(
            ErrorCode::CompatibilityInsufficient,
            "this component does not run the manifest target software version",
            Json::object({{"running", Json(options_.software.to_string())},
                          {"required", Json(target->empty() ? std::string() : *target)}}));
        return response;
      }
      if (*parsed_protocol != options_.protocol) {
        response.status = Status::error(
            ErrorCode::UnsupportedProtocol,
            "this component does not speak the manifest target protocol version",
            Json::object({{"running", Json(options_.protocol.to_string())},
                          {"required", Json(*target_protocol)}}));
        return response;
      }
      if (*target_schema > UINT32_MAX) {
        response.status = malformed("successor prepare target schema is out of range");
        return response;
      }
      const Json* source = body.find("source");
      if (source == nullptr) {
        response.status = invalid_argument("successor prepare requires the source binding");
        return response;
      }
      auto binding = parse_source_binding(*source);
      if (!binding.ok()) {
        response.status = binding.status();
        return response;
      }
      if (binding.value().schema > document_.schema()) {
        response.status = Status::error(
            ErrorCode::MigrationFailure,
            "successor state is already ahead of the source schema",
            Json::object({{"successor_schema", Json(document_.schema().value())},
                          {"source_schema", Json(binding.value().schema.value())}}));
        return response;
      }
      binding.value().handoff = handoff;
      expected_source_ = binding.value();
      current_handoff_ = handoff;
      response.body = Json::object({{"role", Json(*role)},
                                    {"incarnation", ::fabric::evolution::to_json(incarnation_)},
                                    {"source", source_binding_to_json(*expected_source_)}});
      return response;
    }

    // Predecessor preparation: it must actually hold live mutating authority.
    if (!lease_.has_value() || !lease_confirmed_ || lease_->mode != AuthorityMode::Mutating ||
        clock_->now_ms() >= lease_->expires_at_ms) {
      response.status = Status::error(ErrorCode::NotAuthoritative,
                                      "predecessor does not hold live mutating authority");
      return response;
    }
    current_handoff_ = handoff;
    response.body = Json::object({{"role", Json(*role)},
                                  {"incarnation", ::fabric::evolution::to_json(incarnation_)},
                                  {"lsn", Json(lsn_.value())},
                                  {"schema", Json(document_.schema().value())}});
    return response;
  }

  if (request.op == "node.snapshot") {
    HandoffId handoff;
    const Status parsed_handoff = parse_handoff_id(body, handoff);
    if (!parsed_handoff.ok()) {
      response.status = parsed_handoff;
      return response;
    }
    if (lifecycle_ == NodeLifecycle::Retired || lifecycle_ == NodeLifecycle::Unopened) {
      response.status = Status::error(ErrorCode::NotAuthoritative,
                                      "a retired component cannot act as a replication source");
      return response;
    }
    if (!lease_.has_value()) {
      response.status = Status::error(ErrorCode::NotAuthoritative,
                                      "component was never authoritative for this shard");
      return response;
    }
    const auto corrupted = take_fault("node.snapshot.corrupt");
    const auto next_snapshot = last_snapshot_id_.next();
    if (!next_snapshot.has_value()) {
      response.status = Status::error(ErrorCode::BoundsExceeded, "snapshot id space is exhausted");
      return response;
    }
    last_snapshot_id_ = *next_snapshot;
    last_snapshot_lsn_ = lsn_;
    recorded_snapshot_digest_ = snapshot_digest(document_, lsn_);
    Json snapshot = Json::object();
    snapshot.set("id", Json(last_snapshot_id_.value()));
    snapshot.set("lsn", Json(lsn_.value()));
    snapshot.set("schema", Json(document_.schema().value()));
    snapshot.set("digest", Json(recorded_snapshot_digest_.to_hex()));
    Json fields = Json::object();
    for (const auto& entry : document_.fields()) {
      fields.set(entry.first, Json(entry.second));
    }
    if (corrupted.has_value()) {
      // The injected fault changes the image while leaving the declared digest
      // untouched, which is exactly the corruption the successor must detect.
      fields.set("__corrupted__", Json("true"));
    }
    snapshot.set("fields", std::move(fields));
    const Status saved = save_state();
    if (!saved.ok()) {
      response.status = saved;
      return response;
    }
    response.body = Json::object({{"handoff", Json(handoff.value())}, {"snapshot", std::move(snapshot)}});
    return response;
  }

  if (request.op == "node.install_snapshot") {
    HandoffId handoff;
    const Status parsed_handoff = parse_handoff_id(body, handoff);
    if (!parsed_handoff.ok()) {
      response.status = parsed_handoff;
      return response;
    }
    if (!expected_source_.has_value() || expected_source_->handoff != handoff) {
      response.status = Status::error(ErrorCode::IllegalTransition,
                                      "successor was not prepared for this handoff");
      return response;
    }
    const Json* snapshot = body.find("snapshot");
    if (snapshot == nullptr || !snapshot->is_object()) {
      response.status = invalid_argument("install_snapshot requires a snapshot object");
      return response;
    }
    const auto schema = json_u64(*snapshot, "schema");
    const auto snapshot_lsn = json_u64(*snapshot, "lsn");
    const auto digest_text = json_string(*snapshot, "digest");
    if (!schema.has_value() || *schema == 0 || *schema > UINT32_MAX || !snapshot_lsn.has_value() ||
        !digest_text.has_value()) {
      response.status = malformed("snapshot header is incomplete");
      return response;
    }
    auto recorded = Digest::from_hex(*digest_text);
    if (!recorded.has_value()) {
      response.status = malformed("snapshot digest is not a sha256 hex digest");
      return response;
    }
    StateDocument incoming(SchemaVersion::from_value(static_cast<std::uint32_t>(*schema)));
    const Json* fields = snapshot->find("fields");
    if (fields == nullptr || !fields->is_object()) {
      response.status = malformed("snapshot has no fields object");
      return response;
    }
    for (const auto& entry : *fields->try_object()) {
      const std::string* text = entry.second.try_string();
      if (text == nullptr) {
        response.status = malformed("snapshot field is not a string: " + entry.first);
        return response;
      }
      const Status put = incoming.put(entry.first, *text);
      if (!put.ok()) {
        response.status = put;
        return response;
      }
    }
    const Digest computed = snapshot_digest(incoming, LogSequenceNumber::from_value(*snapshot_lsn));
    if (computed != *recorded) {
      response.status = Status::error(
          ErrorCode::IntegrityFailure, "snapshot digest does not match its contents",
          Json::object({{"recorded", Json(recorded->to_hex())}, {"computed", Json(computed.to_hex())}}));
      return response;
    }
    if (incoming.schema() != expected_source_->schema) {
      response.status = Status::error(
          ErrorCode::MigrationFailure, "snapshot schema does not match the declared source generation",
          Json::object({{"snapshot_schema", Json(incoming.schema().value())},
                        {"expected_schema", Json(expected_source_->schema.value())}}));
      return response;
    }
    document_ = std::move(incoming);
    lsn_ = LogSequenceNumber::from_value(*snapshot_lsn);
    operation_id_ = *snapshot_lsn;
    records_.clear();
    const Status truncated = write_bytes_atomic(records_path(), "");
    if (!truncated.ok()) {
      response.status = truncated;
      return response;
    }
    recorded_snapshot_digest_ = computed;
    last_snapshot_lsn_ = lsn_;
    const Status saved = save_state();
    if (!saved.ok()) {
      response.status = saved;
      return response;
    }
    response.body = Json::object({{"lsn", Json(lsn_.value())},
                                  {"schema", Json(document_.schema().value())},
                                  {"digest", Json(computed.to_hex())}});
    return response;
  }

  if (request.op == "node.read_since") {
    const auto from_lsn = json_u64(body, "from_lsn");
    if (!from_lsn.has_value()) {
      response.status = invalid_argument("read_since requires from_lsn");
      return response;
    }
    const auto requested = json_u64_or(body, "max_records", 128);
    const std::size_t limit = static_cast<std::size_t>(
        std::min<std::uint64_t>(requested, options_.max_records_per_response));
    const LogSequenceNumber from = LogSequenceNumber::from_value(*from_lsn);
    const LogSequenceNumber oldest = records_.empty() ? lsn_ : records_.front().lsn;
    const bool truncated = *from_lsn != 0 && from < oldest && !records_.empty() &&
                           records_.front().lsn.value() > from.value() + 1;
    Json entries = Json::array();
    std::size_t emitted = 0;
    for (const ReplicationRecord& record : records_) {
      if (record.lsn.value() <= from.value()) {
        continue;
      }
      if (emitted >= limit) {
        break;
      }
      entries.push_back(record.to_json());
      ++emitted;
    }
    response.body = Json::object({{"records", std::move(entries)},
                                  {"lsn", Json(lsn_.value())},
                                  {"oldest_lsn", Json(oldest.value())},
                                  {"truncated", Json(truncated)}});
    return response;
  }

  if (request.op == "node.apply_records") {
    HandoffId handoff;
    const Status parsed_handoff = parse_handoff_id(body, handoff);
    if (!parsed_handoff.ok()) {
      response.status = parsed_handoff;
      return response;
    }
    if (!expected_source_.has_value() || expected_source_->handoff != handoff) {
      response.status = Status::error(ErrorCode::IllegalTransition,
                                      "successor was not prepared for this handoff");
      return response;
    }
    const Json* source = body.find("source");
    if (source == nullptr) {
      response.status = invalid_argument("apply_records requires the source binding");
      return response;
    }
    auto binding = parse_source_binding(*source);
    if (!binding.ok()) {
      response.status = binding.status();
      return response;
    }
    SourceBinding presented = binding.take();
    presented.handoff = handoff;
    if (!presented.matches(*expected_source_)) {
      response.status = Status::error(
          ErrorCode::StaleGeneration,
          "replicated state is not bound to the generation the successor was prepared for",
          Json::object({{"presented", source_binding_to_json(presented)},
                        {"expected", source_binding_to_json(*expected_source_)}}));
      return response;
    }
    const Json* entries = body.find("records");
    if (entries == nullptr || !entries->is_array()) {
      response.status = invalid_argument("apply_records requires a records array");
      return response;
    }
    if (entries->size() > options_.max_records_per_response) {
      response.status = Status::error(ErrorCode::BoundsExceeded,
                                      "apply_records batch exceeds the permitted size",
                                      Json::object({{"records", Json(static_cast<std::uint64_t>(entries->size()))},
                                                    {"limit", Json(static_cast<std::uint64_t>(
                                                                  options_.max_records_per_response))}}));
      return response;
    }
    std::size_t applied = 0;
    for (std::size_t index = 0; index < entries->size(); ++index) {
      auto record = ReplicationRecord::from_json(entries->at(index));
      if (!record.ok()) {
        response.status = record.status();
        return response;
      }
      const ReplicationRecord& entry = record.value();
      if (!(entry.origin == expected_source_->source)) {
        response.status = Status::error(ErrorCode::StaleIncarnation,
                                        "replicated record originates from an unexpected incarnation",
                                        Json::object({{"origin", Json(entry.origin.to_string())},
                                                      {"expected", Json(expected_source_->source.to_string())}}));
        return response;
      }
      const auto expected_lsn = lsn_.next();
      if (!expected_lsn.has_value() || entry.lsn != *expected_lsn) {
        response.status = Status::error(
            ErrorCode::NotReady, "replicated record is not the next expected log position",
            Json::object({{"record_lsn", Json(entry.lsn.value())},
                          {"expected_lsn", Json(lsn_.value() + 1)}}));
        return response;
      }
      const Status put = document_.put(entry.key, entry.value);
      if (!put.ok()) {
        response.status = put;
        return response;
      }
      lsn_ = entry.lsn;
      operation_id_ += 1;
      const Status appended = append_record_entry(entry);
      if (!appended.ok()) {
        response.status = appended;
        return response;
      }
      ++applied;
    }
    const Status saved = save_state();
    if (!saved.ok()) {
      response.status = saved;
      return response;
    }
    response.body = Json::object({{"applied", Json(static_cast<std::uint64_t>(applied))},
                                  {"lsn", Json(lsn_.value())}});
    return response;
  }

  if (request.op == "node.readiness") {
    HandoffId handoff;
    const Status parsed_handoff = parse_handoff_id(body, handoff);
    if (!parsed_handoff.ok()) {
      response.status = parsed_handoff;
      return response;
    }
    if (!expected_source_.has_value() || expected_source_->handoff != handoff) {
      response.status = Status::error(ErrorCode::IllegalTransition,
                                      "successor was not prepared for this handoff");
      return response;
    }
    const auto target_lsn = json_u64_or(body, "target_lsn", 0);
    const auto target_schema = json_u64_or(body, "target_schema", 0);
    std::string reason = "ready";
    bool ready = true;
    if (document_.schema().value() != static_cast<std::uint32_t>(target_schema)) {
      ready = false;
      reason = "successor state has not been migrated to the manifest target schema";
    } else if (target_lsn != 0 && lsn_.value() < target_lsn) {
      ready = false;
      reason = "successor has not caught up to the snapshot target log position";
    } else if (lifecycle_ == NodeLifecycle::Retired || lifecycle_ == NodeLifecycle::Fenced) {
      ready = false;
      reason = "successor is not in a serving lifecycle state";
    }
    response.body = Json::object({{"ready", Json(ready)},
                                  {"reason", Json(reason)},
                                  {"lsn", Json(lsn_.value())},
                                  {"schema", Json(document_.schema().value())},
                                  {"digest", Json(snapshot_digest(document_, lsn_).to_hex())}});
    return response;
  }

  if (request.op == "node.fence") {
    HandoffId handoff;
    const Status parsed_handoff = parse_handoff_id(body, handoff);
    if (!parsed_handoff.ok()) {
      response.status = parsed_handoff;
      return response;
    }
    const auto fence_id = json_u64(body, "fence_id");
    const Json* target = body.find("target");
    if (!fence_id.has_value() || *fence_id == 0 || target == nullptr) {
      response.status = invalid_argument("fence requires fence_id and target");
      return response;
    }
    auto parsed_target = incarnation_from_json(*target);
    if (!parsed_target.ok()) {
      response.status = parsed_target.status();
      return response;
    }
    if (parsed_target.value() != incarnation_) {
      response.status = Status::error(
          ErrorCode::StaleIncarnation, "fence names a different incarnation than this process",
          Json::object({{"target", Json(parsed_target.value().to_string())},
                        {"current", Json(incarnation_.to_string())}}));
      return response;
    }
    const FenceId id = FenceId::from_value(*fence_id);
    if (lifecycle_ == NodeLifecycle::Fenced && last_fence_id_ == id) {
      response.body = Json::object({{"acknowledged", Json(true)}, {"idempotent", Json(true)},
                                    {"fence_id", Json(id.value())}});
      return response;
    }
    lifecycle_ = NodeLifecycle::Fenced;
    lease_confirmed_ = false;
    if (lease_.has_value()) {
      lease_->state = LeaseState::Fenced;
      lease_->attested = false;
    }
    last_fence_id_ = id;
    const Status saved = save_state();
    if (!saved.ok()) {
      response.status = saved;
      return response;
    }
    response.body = Json::object({{"acknowledged", Json(true)},
                                  {"idempotent", Json(false)},
                                  {"fence_id", Json(id.value())},
                                  {"incarnation", ::fabric::evolution::to_json(incarnation_)},
                                  {"lsn", Json(lsn_.value())}});
    return response;
  }

  if (request.op == "node.grant_authority") {
    HandoffId handoff;
    const Status parsed_handoff = parse_handoff_id(body, handoff);
    if (!parsed_handoff.ok()) {
      response.status = parsed_handoff;
      return response;
    }
    if (lifecycle_ == NodeLifecycle::Retired) {
      response.status = Status::error(ErrorCode::NotAuthoritative, "node is retired");
      return response;
    }
    const auto epoch = json_u64(body, "epoch");
    const auto generation = json_u64(body, "generation");
    const auto token_text = json_string(body, "token");
    const auto ttl = json_u64_or(body, "ttl_ms", options_.lease_ttl_ms);
    const auto mode_text = json_string(body, "mode");
    if (!epoch.has_value() || *epoch == 0 || !generation.has_value() || *generation == 0 ||
        !token_text.has_value() || !mode_text.has_value()) {
      response.status = invalid_argument("grant_authority requires epoch, generation, token and mode");
      return response;
    }
    auto token = AuthorityToken::parse(*token_text);
    if (!token.has_value() || token->is_nil()) {
      response.status = malformed("grant_authority token is not a valid uuid");
      return response;
    }
    AuthorityMode mode = AuthorityMode::Mutating;
    if (*mode_text == "read_only_shared") {
      mode = AuthorityMode::ReadOnlyShared;
    } else if (*mode_text == "mutating") {
      mode = AuthorityMode::Mutating;
    } else {
      response.status = malformed("grant_authority mode is invalid");
      return response;
    }
    if (ttl == 0 || ttl > 24ULL * 60ULL * 60ULL * 1000ULL) {
      response.status = Status::error(ErrorCode::BoundsExceeded, "grant_authority ttl is out of range");
      return response;
    }
    AuthorityLease lease;
    lease.shard = options_.shard;
    lease.holder = incarnation_;
    lease.epoch = EpochNumber::from_value(*epoch);
    lease.generation = Generation::from_value(*generation);
    lease.token = *token;
    lease.mode = mode;
    lease.state = LeaseState::Active;
    lease.issued_at_ms = clock_->now_ms();
    lease.expires_at_ms = clock_->now_ms() + ttl;
    lease.attested = true;
    lease.attestation_seq = 1;
    lease.handoff = handoff;
    lease.reason = "granted by the evolution controller";
    lease_ = lease;
    lease_confirmed_ = true;
    lifecycle_ = NodeLifecycle::Serving;
    const Status saved = save_state();
    if (!saved.ok()) {
      response.status = saved;
      return response;
    }
    response.body = Json::object({{"incarnation", ::fabric::evolution::to_json(incarnation_)},
                                  {"epoch", Json(lease.epoch.value())},
                                  {"generation", Json(lease.generation.value())},
                                  {"expires_at_ms", Json(lease.expires_at_ms)},
                                  {"mode", Json(std::string(::fabric::evolution::to_string(lease.mode)))}});
    return response;
  }

  if (request.op == "node.reattest") {
    const auto token_text = json_string(body, "token");
    if (!token_text.has_value()) {
      response.status = invalid_argument("reattest requires a token");
      return response;
    }
    auto token = AuthorityToken::parse(*token_text);
    if (!token.has_value()) {
      response.status = malformed("reattest token is not a valid uuid");
      return response;
    }
    if (!lease_.has_value() || lease_->token != *token) {
      response.status = Status::error(ErrorCode::StaleAuthority,
                                      "reattest token does not match the recorded lease");
      return response;
    }
    if (clock_->now_ms() >= lease_->expires_at_ms) {
      response.status = Status::error(ErrorCode::StaleAuthority, "cannot re-attest an expired lease");
      return response;
    }
    lease_->attested = true;
    lease_->attestation_seq += 1;
    lease_confirmed_ = true;
    response.body = Json::object({{"attested", Json(true)},
                                  {"incarnation", ::fabric::evolution::to_json(incarnation_)},
                                  {"generation", Json(lease_->generation.value())}});
    return response;
  }

  if (request.op == "node.migrate") {
    HandoffId handoff;
    const Status parsed_handoff = parse_handoff_id(body, handoff);
    if (!parsed_handoff.ok()) {
      response.status = parsed_handoff;
      return response;
    }
    const Json* steps = body.find("steps");
    if (steps == nullptr || !steps->is_array() || steps->size() == 0) {
      response.status = invalid_argument("migrate requires a non-empty steps array");
      return response;
    }
    if (steps->size() > 64) {
      response.status = Status::error(ErrorCode::BoundsExceeded, "migrate accepts at most 64 steps");
      return response;
    }
    std::vector<MigrationStepSpec> path;
    for (std::size_t index = 0; index < steps->size(); ++index) {
      auto step = migration_step_from_json(steps->at(index));
      if (!step.ok()) {
        response.status = step.status();
        return response;
      }
      path.push_back(step.take());
    }
    StateMigrator migrator;
    StateDocument working = document_;
    auto outcome = migrator.migrate_forward(working, path);
    if (!outcome.ok()) {
      response.status = outcome.status();
      return response;
    }
    document_ = std::move(working);
    for (const MigrationStepRecord& record : outcome.value().steps) {
      MigrationJournalRecord entry;
      entry.shard = options_.shard;
      entry.component = options_.component;
      entry.step = record.step;
      entry.from = record.from;
      entry.to = record.to;
      entry.state_digest_after = record.state_digest_after;
      entry.applied_at_ms = clock_->now_ms();
      entry.irreversible_boundary = record.irreversible_boundary;
      const Status recorded = journal_.record(entry);
      if (!recorded.ok()) {
        response.status = recorded;
        return response;
      }
    }
    const Status saved = save_state();
    if (!saved.ok()) {
      response.status = saved;
      return response;
    }
    Json applied = Json::array();
    for (const MigrationStepSpec& step : path) {
      applied.push_back(Json(step.id.str()));
    }
    response.body = Json::object(
        {{"schema", Json(document_.schema().value())},
         {"state_digest_after", Json(outcome.value().state_digest_after.to_hex())},
         {"crossed_irreversible_boundary", Json(outcome.value().crossed_irreversible_boundary)},
         {"steps", Json(static_cast<std::uint64_t>(outcome.value().steps.size()))},
         {"applied", std::move(applied)},
         {"handoff", Json(handoff.value())}});
    return response;
  }

  if (request.op == "node.retire") {
    HandoffId handoff;
    const Status parsed_handoff = parse_handoff_id(body, handoff);
    if (!parsed_handoff.ok()) {
      response.status = parsed_handoff;
      return response;
    }
    lifecycle_ = NodeLifecycle::Retired;
    retired_marker_ = true;
    lease_confirmed_ = false;
    if (lease_.has_value()) {
      lease_->state = LeaseState::Revoked;
      lease_->attested = false;
    }
    const Status saved = save_state();
    if (!saved.ok()) {
      response.status = saved;
      return response;
    }
    response.body = Json::object({{"retired", Json(true)}, {"handoff", Json(handoff.value())}});
    return response;
  }

  if (request.op == "node.write") {
    const Json* claim_value = body.find("claim");
    if (claim_value == nullptr) {
      response.status = invalid_argument("node.write requires an authority claim");
      return response;
    }
    auto claim = authority_claim_from_json(*claim_value);
    if (!claim.ok()) {
      response.status = claim.status();
      return response;
    }
    const Status authority = require_authority(claim.value(), AuthorityMode::Mutating, "write");
    if (!authority.ok()) {
      ++writes_refused_;
      response.status = authority;
      return response;
    }
    const auto key = json_string(body, "key");
    const auto value = json_string(body, "value");
    if (!key.has_value() || key->empty() || !value.has_value()) {
      response.status = invalid_argument("node.write requires key and value");
      return response;
    }
    const Status applied = apply_write(*key, *value, lease_->epoch, lease_->generation, incarnation_);
    if (!applied.ok()) {
      response.status = applied;
      return response;
    }
    ++writes_applied_;
    response.body = Json::object({{"lsn", Json(lsn_.value())},
                                  {"epoch", Json(lease_->epoch.value())},
                                  {"generation", Json(lease_->generation.value())},
                                  {"incarnation", ::fabric::evolution::to_json(incarnation_)}});
    return response;
  }

  if (request.op == "node.read") {
    const auto key = json_string(body, "key");
    if (!key.has_value() || key->empty()) {
      response.status = invalid_argument("node.read requires a key");
      return response;
    }
    const bool allow_stale = json_bool_or(body, "allow_stale", false);
    const bool live = lease_.has_value() && lease_confirmed_ &&
                      clock_->now_ms() < lease_->expires_at_ms;
    if (!allow_stale) {
      if (lifecycle_ == NodeLifecycle::Retired || lifecycle_ == NodeLifecycle::Fenced) {
        response.status = Status::error(ErrorCode::NotAuthoritative,
                                        "node is not serving authoritative reads");
        return response;
      }
      if (!live || !lease_.has_value() || !authority_allows_read(lease_->mode)) {
        response.status = Status::error(ErrorCode::NotAuthoritative,
                                        "node does not hold a live read-capable lease");
        return response;
      }
    }
    const auto value = document_.get(*key);
    ++reads_served_;
    Json out = Json::object();
    out.set("found", Json(value.has_value()));
    out.set("value", Json(value.has_value() ? *value : std::string()));
    out.set("lsn", Json(lsn_.value()));
    out.set("authoritative", Json(live && lease_.has_value() &&
                                  authority_allows_read(lease_->mode) &&
                                  lifecycle_ == NodeLifecycle::Serving));
    out.set("lifecycle", Json(std::string(lifecycle_name(lifecycle_))));
    out.set("incarnation", ::fabric::evolution::to_json(incarnation_));
    out.set("epoch", Json(lease_.has_value() ? lease_->epoch.value() : 0));
    out.set("generation", Json(lease_.has_value() ? lease_->generation.value() : 0));
    out.set("schema", Json(document_.schema().value()));
    out.set("state_digest", Json(snapshot_digest(document_, lsn_).to_hex()));
    response.body = std::move(out);
    return response;
  }

  response.status = Status::error(ErrorCode::NotFound, "unknown node operation: " + request.op);
  return response;
}

}  // namespace fabric::evolution
