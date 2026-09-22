// Fabric Evolution — the evolution controller (implementation).
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric/evolution/controller.hpp"

#include <algorithm>
#include <fstream>
#include <random>
#include <sstream>

namespace fabric::evolution {
namespace {

constexpr std::uint64_t kMaxHandoffRestarts = 8;

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

[[nodiscard]] Result<NodeReport> report_from_body(const Json& body) {
  const Json* report = body.find("report");
  if (report == nullptr || !report->is_object()) {
    return malformed("node report is missing");
  }
  NodeReport out;
  const auto component = json_string(*report, "component");
  const auto shard = json_string(*report, "shard");
  if (!component.has_value() || !shard.has_value()) {
    return malformed("node report is missing component or shard");
  }
  auto parsed_component = ComponentId::parse(*component);
  auto parsed_shard = ShardId::parse(*shard);
  if (!parsed_component.has_value() || !parsed_shard.has_value()) {
    return malformed("node report identity is invalid");
  }
  out.component = *parsed_component;
  out.shard = *parsed_shard;
  const Json* incarnation = report->find("incarnation");
  if (incarnation == nullptr) {
    return malformed("node report is missing incarnation");
  }
  auto parsed_incarnation = incarnation_from_json(*incarnation);
  if (!parsed_incarnation.ok()) {
    return parsed_incarnation.status();
  }
  out.incarnation = parsed_incarnation.value();
  if (const auto software = json_string(*report, "software")) {
    if (auto parsed = SoftwareVersion::parse(*software)) {
      out.software = *parsed;
    }
  }
  if (const auto protocol = json_string(*report, "protocol")) {
    if (auto parsed = ProtocolVersion::parse(*protocol)) {
      out.protocol = *parsed;
    }
  }
  const auto schema = json_u64(*report, "schema");
  if (schema.has_value() && *schema > 0 && *schema <= UINT32_MAX) {
    out.schema = SchemaVersion::from_value(static_cast<std::uint32_t>(*schema));
  }
  out.epoch = EpochNumber::from_value(json_u64_or(*report, "epoch", 0));
  out.generation = Generation::from_value(json_u64_or(*report, "generation", 0));
  out.lsn = LogSequenceNumber::from_value(json_u64_or(*report, "lsn", 0));
  out.lease_expires_at_ms = json_u64_or(*report, "lease_expires_at_ms", 0);
  out.has_authority = json_bool_or(*report, "has_authority", false);
  out.lease_confirmed = json_bool_or(*report, "lease_confirmed", false);
  out.writes_applied = json_u64_or(*report, "writes_applied", 0);
  out.writes_refused = json_u64_or(*report, "writes_refused", 0);
  out.reads_served = json_u64_or(*report, "reads_served", 0);
  if (const auto mode = json_string(*report, "mode")) {
    if (*mode == "mutating") {
      out.mode = AuthorityMode::Mutating;
    } else if (*mode == "read_only_shared") {
      out.mode = AuthorityMode::ReadOnlyShared;
    } else {
      out.mode = AuthorityMode::None;
    }
  }
  if (const auto token = json_string(*report, "token")) {
    if (auto parsed = AuthorityToken::parse(*token)) {
      out.token = *parsed;
    }
  }
  if (const auto digest = json_string(*report, "state_digest")) {
    if (auto parsed = Digest::from_hex(*digest)) {
      out.state_digest = *parsed;
    }
  }
  if (const auto lifecycle = json_string(*report, "lifecycle")) {
    if (*lifecycle == "serving") {
      out.lifecycle = NodeLifecycle::Serving;
    } else if (*lifecycle == "recovering") {
      out.lifecycle = NodeLifecycle::Recovering;
    } else if (*lifecycle == "fenced") {
      out.lifecycle = NodeLifecycle::Fenced;
    } else if (*lifecycle == "retired") {
      out.lifecycle = NodeLifecycle::Retired;
    } else if (*lifecycle == "stopped") {
      out.lifecycle = NodeLifecycle::Stopped;
    } else {
      out.lifecycle = NodeLifecycle::Unopened;
    }
  }
  return out;
}

[[nodiscard]] AuthorityClaim claim_of(const AuthorityLease& lease) {
  AuthorityClaim claim;
  claim.shard = lease.shard;
  claim.incarnation = lease.holder;
  claim.epoch = lease.epoch;
  claim.generation = lease.generation;
  claim.token = lease.token;
  return claim;
}

[[nodiscard]] Json source_binding_json(const AuthorityClaim& claim, SchemaVersion schema,
                                       HandoffId handoff) {
  Json out = Json::object();
  out.set("incarnation", ::fabric::evolution::to_json(claim.incarnation));
  out.set("epoch", Json(claim.epoch.value()));
  out.set("generation", Json(claim.generation.value()));
  out.set("token", Json(claim.token.to_compact_string()));
  out.set("schema", Json(schema.value()));
  out.set("handoff", Json(handoff.value()));
  return out;
}

}  // namespace

NodeClient::NodeClient(EndpointRef endpoint, ServerOptions options)
    : endpoint_(std::move(endpoint)), options_(std::move(options)) {}

Result<Json> NodeClient::call(const std::string& op, Json body) const {
  RpcRequest request;
  request.id = "controller-call";
  request.op = op;
  request.body = std::move(body);
  auto response = rpc_call(endpoint_.host, endpoint_.port, request, options_);
  if (!response.ok()) {
    return Status::error(response.status().code(),
                         "call to " + endpoint_.component.str() + " failed: " +
                             response.status().message(),
                         response.status().detail());
  }
  if (!response.value().status.ok()) {
    return response.value().status;
  }
  return response.value().body;
}

Result<NodeReport> NodeClient::report() const {
  auto body = call("node.status", Json::object());
  if (!body.ok()) {
    return body.status();
  }
  return report_from_body(body.value());
}

Result<ProtocolHello> NodeClient::hello() const {
  auto body = call("node.hello", Json::object());
  if (!body.ok()) {
    return body.status();
  }
  const Json* hello = body.value().find("hello");
  if (hello == nullptr) {
    return malformed("node hello response is missing the hello document");
  }
  return protocol_hello_from_json(*hello);
}

EvolutionController::EvolutionController(ControllerOptions options, CompatibilityRegistry& registry,
                                         Clock& clock)
    : options_(std::move(options)),
      registry_(&registry),
      clock_(&clock),
      authority_(clock),
      epoch_log_(options_.state_directory + "/epochs.fxr", kDefaultEpochLogCapacity),
      window_start_lsn_(LogSequenceNumber::from_value(0)) {}

EvolutionController::~EvolutionController() { stop(); }

Result<Json> EvolutionController::completed(const Status& status) const {
  if (!status.ok()) {
    return status;
  }
  return status_json_locked();
}

std::string EvolutionController::state_path() const {
  return options_.state_directory + "/controller.fxi";
}

Status EvolutionController::load_state() {
  if (!path_exists(state_path())) {
    return Status::success();
  }
  auto recovered = read_integrity_file_with_recovery(state_path(), true);
  if (!recovered.ok()) {
    return Status::error(recovered.status().code(),
                         "controller state is damaged and cannot be recovered: " +
                             recovered.status().message(),
                         recovered.status().detail());
  }
  recovered_from_backup_ = recovered.value().from_backup;
  const JsonParseResult parsed = parse_json(recovered.value().payload);
  if (!parsed.ok()) {
    return Status::error(ErrorCode::IntegrityFailure,
                         "controller state is not valid JSON: " + parsed.error);
  }
  const Json& document = *parsed.value;

  const Json* authority = document.find("authority");
  if (authority == nullptr) {
    return Status::error(ErrorCode::IntegrityFailure, "controller state has no authority section");
  }
  const Status loaded = authority_.load(*authority);
  if (!loaded.ok()) {
    return loaded;
  }

  if (const Json* manifest = document.find("manifest")) {
    auto parsed_manifest = manifest_from_json(*manifest);
    if (!parsed_manifest.ok()) {
      return parsed_manifest.status();
    }
    manifest_ = parsed_manifest.take();
  }
  if (const Json* campaign = document.find("campaign")) {
    auto parsed_campaign = campaign_record_from_json(*campaign);
    if (!parsed_campaign.ok()) {
      return parsed_campaign.status();
    }
    campaign_ = parsed_campaign.take();
  }
  if (const Json* handoff = document.find("handoff")) {
    auto parsed_handoff = handoff_record_from_json(*handoff);
    if (!parsed_handoff.ok()) {
      return parsed_handoff.status();
    }
    handoff_ = parsed_handoff.take();
  }
  if (manifest_.has_value()) {
    source_endpoint_ =
        EndpointRef{manifest_->source.id, manifest_->source.host, manifest_->source.port, "source"};
    target_endpoint_ =
        EndpointRef{manifest_->target.id, manifest_->target.host, manifest_->target.port, "target"};
  }
  if (const Json* claim = document.find("fenced_predecessor_claim")) {
    auto parsed_claim = authority_claim_from_json(*claim);
    if (parsed_claim.ok()) {
      fenced_predecessor_claim_ = parsed_claim.take();
    }
  }
  next_handoff_id_ = json_u64_or(document, "next_handoff_id", 1);
  next_epoch_ = EpochNumber::from_value(json_u64_or(document, "next_epoch", 1));
  window_start_lsn_ = LogSequenceNumber::from_value(json_u64_or(document, "window_start_lsn", 0));
  last_deferral_reason_ = json_string_or(document, "last_deferral_reason", "");
  last_pause_deferral_ms_ = json_u64_or(document, "last_pause_deferral_ms", 0);

  const Json* incarnation = document.find("controller_incarnation");
  if (incarnation != nullptr) {
    auto parsed_incarnation = incarnation_from_json(*incarnation);
    if (parsed_incarnation.ok()) {
      controller_incarnation_ = parsed_incarnation.value();
      controller_boot_ = parsed_incarnation.value().boot();
    }
  }
  return Status::success();
}

Status EvolutionController::save_state() {
  Json out = Json::object();
  out.set("format", Json(1));
  out.set("controller", Json(options_.controller_id.str()));
  out.set("controller_incarnation", ::fabric::evolution::to_json(controller_incarnation_));
  out.set("authority", authority_.to_json());
  out.set("next_handoff_id", Json(next_handoff_id_));
  out.set("next_epoch", Json(next_epoch_.value()));
  out.set("window_start_lsn", Json(window_start_lsn_.value()));
  out.set("last_deferral_reason", Json(last_deferral_reason_));
  out.set("last_pause_deferral_ms", Json(last_pause_deferral_ms_));
  if (manifest_.has_value()) {
    out.set("manifest", ::fabric::evolution::to_json(*manifest_));
  }
  if (campaign_.has_value()) {
    out.set("campaign", ::fabric::evolution::to_json(*campaign_));
  }
  if (handoff_.has_value()) {
    out.set("handoff", ::fabric::evolution::to_json(*handoff_));
  }
  if (fenced_predecessor_claim_.has_value()) {
    out.set("fenced_predecessor_claim", ::fabric::evolution::to_json(*fenced_predecessor_claim_));
  }
  return write_integrity_file(state_path(), out.dump(), true);
}

Status EvolutionController::open() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (options_.state_directory.empty()) {
    return invalid_argument("controller state directory must be set");
  }
  if (!options_.controller_id.is_valid()) {
    return invalid_argument("controller identity must be valid");
  }
  const Status directory = ensure_directory(options_.state_directory);
  if (!directory.ok()) {
    return directory;
  }
  BootCounter boot = BootCounter::from_value(1);
  const std::string identity = options_.state_directory + "/controller-identity.fxi";
  if (path_exists(identity)) {
    auto raw = read_integrity_file(identity, nullptr);
    if (!raw.ok()) {
      return Status::error(raw.status().code(),
                           "durable controller identity is damaged; refusing to guess: " +
                               raw.status().message(),
                           raw.status().detail());
    }
    const JsonParseResult parsed = parse_json(raw.value());
    if (!parsed.ok()) {
      return Status::error(ErrorCode::IntegrityFailure, "controller identity is not valid JSON");
    }
    const auto previous = json_u64(*parsed.value, "boot_counter");
    if (!previous.has_value()) {
      return Status::error(ErrorCode::IntegrityFailure, "controller identity has no boot counter");
    }
    const auto next = BootCounter::from_value(*previous).next();
    if (!next.has_value()) {
      return Status::error(ErrorCode::BoundsExceeded, "controller boot counter is exhausted");
    }
    boot = *next;
  }
  Json identity_document = Json::object();
  identity_document.set("controller", Json(options_.controller_id.str()));
  identity_document.set("boot_counter", Json(boot.value()));
  const Status saved_identity = write_integrity_file(identity, identity_document.dump(), false);
  if (!saved_identity.ok()) {
    return saved_identity;
  }

  const Status loaded = load_state();
  if (!loaded.ok()) {
    return loaded;
  }
  if (!controller_incarnation_.is_valid() || controller_incarnation_.boot() != boot) {
    controller_incarnation_ =
        IncarnationId(options_.controller_id, IncarnationUuid::from_value(random_uuid()), boot);
  }
  const Status replayed = epoch_log_.replay();
  if (!replayed.ok()) {
    return replayed;
  }
  if (campaign_.has_value() && campaign_->state == CampaignState::Preflighting) {
    // A controller that died during preflight restarts preflight rather than
    // assuming the earlier attempt completed.
    campaign_->state = CampaignState::Planned;
  }
  return save_state();
}

Status EvolutionController::start_admin() {
  ServerOptions server_options;
  server_options.host = options_.host;
  server_options.port = options_.port;
  server_options.worker_threads = options_.worker_threads;
  server_options.io_deadline_ms = options_.io_deadline_ms;
  auto server = std::make_unique<FabricServer>(
      server_options, [this](const RpcRequest& request, const RequestContext&) {
        RpcResponse response;
        response.id = request.id;
        response.op = request.op;
        auto result = admin(request);
        if (result.ok()) {
          response.body = result.take();
        } else {
          response.status = result.status();
        }
        return response;
      });
  const Status started = server->start();
  if (!started.ok()) {
    return started;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  port_ = server->port();
  server_ = std::move(server);
  return Status::success();
}

void EvolutionController::stop() {
  std::unique_ptr<FabricServer> server;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    server = std::move(server_);
    port_ = 0;
  }
  if (server != nullptr) {
    server->stop();
  }
}

DecisionAuthority EvolutionController::decision_authority() const {
  DecisionAuthority authority;
  if (manifest_.has_value() && authority_.has_shard(manifest_->shard)) {
    authority.epoch = authority_.current_epoch(manifest_->shard).value_or(EpochNumber::invalid());
    authority.generation =
        authority_.current_generation(manifest_->shard).value_or(Generation::invalid());
  }
  authority.actor = controller_incarnation_;
  return authority;
}

Digest EvolutionController::current_manifest_digest() const {
  return manifest_.has_value() ? manifest_->digest : Digest{};
}

std::optional<CampaignRecord> EvolutionController::campaign() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return campaign_;
}

std::optional<HandoffRecord> EvolutionController::handoff() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return handoff_;
}

std::optional<EvolutionManifest> EvolutionController::manifest() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return manifest_;
}

void EvolutionController::set_manifest_for_test(EvolutionManifest manifest) {
  std::lock_guard<std::mutex> lock(mutex_);
  manifest_ = std::move(manifest);
}

WindowStatus EvolutionController::window_status_locked() const {
  if (!campaign_.has_value() || !manifest_.has_value()) {
    WindowStatus status;
    status.reason = "no campaign is loaded";
    return status;
  }
  CampaignRecord observed = *campaign_;
  LogSequenceNumber highest = window_start_lsn_;
  if (last_source_report_.has_value() && last_source_report_->lsn > highest) {
    highest = last_source_report_->lsn;
  }
  if (last_target_report_.has_value() && last_target_report_->lsn > highest) {
    highest = last_target_report_->lsn;
  }
  if (observed.window_started_at_ms != 0 && highest.value() > window_start_lsn_.value()) {
    observed.operations_in_window = highest.value() - window_start_lsn_.value();
  }
  return evaluate_window(observed, manifest_->effective_window(), clock_->now_ms());
}

Result<Json> EvolutionController::plan_locked(const EvolutionManifest& manifest, Explanation& explanation) {
  explanation = Explanation("campaign.plan", "fabric-evolution.manifest-admission.v1");
  explanation.set_decided_at(clock_->now_ms());
  explanation.set_authority(decision_authority());
  Json inputs = Json::object();
  inputs.set("manifest", Json(manifest.id.str()));
  inputs.set("campaign", Json(manifest.campaign.str()));
  inputs.set("shard", Json(manifest.shard.str()));
  inputs.set("source", Json(manifest.source.software.to_string()));
  inputs.set("target", Json(manifest.target.software.to_string()));
  inputs.set("source_schema", Json(manifest.source.schema.value()));
  inputs.set("target_schema", Json(manifest.target.schema.value()));
  explanation.set_inputs(std::move(inputs));

  if (campaign_.has_value() && !is_terminal_state(campaign_->state)) {
    explanation.set_outcome(DecisionOutcome::Rejected);
    explanation.set_selected_action("refuse_plan");
    explanation.add_rejected_alternative("replace_running_campaign",
                                         "a campaign is already in a non-terminal state");
    explanation.set_resulting_state(std::string(::fabric::evolution::to_string(campaign_->state)));
    return Status::error(ErrorCode::Conflict, "a campaign is already in progress",
                         ::fabric::evolution::to_json(*campaign_));
  }

  EvolutionManifest sealed = manifest;
  const Status admission = sealed.digest.is_zero() ? sealed.seal() : sealed.validate();
  if (!admission.ok()) {
    explanation.set_outcome(DecisionOutcome::Rejected);
    explanation.set_selected_action("refuse_plan");
    explanation.add_rejected_alternative("admit_manifest", admission.message());
    return admission;
  }
  if (!sealed.digest.is_zero()) {
    const Status verified = sealed.verify_digest();
    if (!verified.ok()) {
      explanation.set_outcome(DecisionOutcome::Rejected);
      explanation.set_selected_action("refuse_plan");
      explanation.add_rejected_alternative("admit_manifest", verified.message());
      return verified;
    }
  }

  explanation.set_evidence(::fabric::evolution::to_json(sealed.compatibility));
  const EvidenceCheck evidence =
      verify_evidence(sealed.compatibility, *registry_, options_.evidence_policy);
  if (!evidence.satisfied) {
    explanation.set_outcome(DecisionOutcome::Rejected);
    explanation.set_selected_action("refuse_plan");
    explanation.add_rejected_alternative("admit_manifest", evidence.rationale);
    for (const std::string& difference : evidence.differences) {
      explanation.add_note("evidence difference: " + difference);
    }
    return Status::error(ErrorCode::CompatibilityInsufficient,
                         "compatibility evidence does not satisfy the configured policy",
                         Json::object({{"rationale", Json(evidence.rationale)}}));
  }

  if (!authority_.has_shard(sealed.shard)) {
    const Status established = authority_.establish(sealed.shard, next_epoch_);
    if (!established.ok()) {
      return established;
    }
    EpochEvent event;
    event.kind = EpochEventKind::ShardEstablished;
    event.shard = sealed.shard;
    event.epoch = next_epoch_;
    event.generation = Generation::from_value(1);
    event.at_ms = clock_->now_ms();
    event.payload = Json::object({{"controller", Json(options_.controller_id.str())}});
    const Status logged = epoch_log_.append(event);
    if (!logged.ok()) {
      return logged;
    }
  }

  manifest_ = sealed;
  CampaignRecord record;
  record.id = sealed.campaign;
  record.manifest = sealed.id;
  record.manifest_digest = sealed.digest;
  record.shard = sealed.shard;
  record.state = CampaignState::Planned;
  record.epoch = authority_.current_epoch(sealed.shard).value_or(EpochNumber::invalid());
  record.generation = authority_.current_generation(sealed.shard).value_or(Generation::invalid());
  record.updated_at_ms = clock_->now_ms();
  record.detail = "planned";
  record.events.push_back(CampaignEvent{CampaignState::Planned, CampaignState::Planned,
                                        CampaignAction::Plan, clock_->now_ms(), "manifest admitted"});
  campaign_ = record;
  source_endpoint_ = EndpointRef{sealed.source.id, sealed.source.host, sealed.source.port, "source"};
  target_endpoint_ = EndpointRef{sealed.target.id, sealed.target.host, sealed.target.port, "target"};

  const Status saved = save_state();
  if (!saved.ok()) {
    return saved;
  }
  explanation.set_outcome(DecisionOutcome::Accepted);
  explanation.set_selected_action("admit_manifest");
  explanation.set_resulting_state("planned");
  explanation.add_rejected_alternative("refuse_manifest", "no admission rule rejected the manifest");
  return status_json_locked();
}

Status EvolutionController::apply_action(CampaignAction action, Explanation& explanation) {
  if (!campaign_.has_value() || !manifest_.has_value()) {
    return Status::error(ErrorCode::NotFound, "no campaign has been planned");
  }
  const bool irreversible =
      campaign_->irreversible_boundary_crossed ||
      (handoff_.has_value() && handoff_->reached(HandoffPhase::AuthorityTransferred));
  HandoffRecord handoff_view;
  if (handoff_.has_value()) {
    handoff_view = *handoff_;
  }
  const ActionDecision decision =
      evaluate_campaign_action(*campaign_, action, handoff_view, irreversible, manifest_->id);
  explanation.set_inputs(
      Json::object({{"campaign_state",
                     Json(std::string(::fabric::evolution::to_string(campaign_->state)))},
                    {"action", Json(std::string(::fabric::evolution::to_string(action)))},
                    {"handoff_phase", Json(std::string(::fabric::evolution::to_string(
                                         handoff_view.phase)))}}));
  if (!decision.accepted && !decision.deferred) {
    explanation.set_outcome(DecisionOutcome::Rejected);
    explanation.set_selected_action("refuse_" + std::string(::fabric::evolution::to_string(action)));
    explanation.add_rejected_alternative(std::string(::fabric::evolution::to_string(action)),
                                         decision.reason);
    return Status::error(ErrorCode::IllegalTransition, decision.reason,
                         Json::object({{"rule", Json(decision.rule)}}));
  }
  if (decision.deferred) {
    explanation.set_outcome(DecisionOutcome::Deferred);
    explanation.set_selected_action("defer_" + std::string(::fabric::evolution::to_string(action)));
    last_deferral_reason_ = decision.reason;
    last_pause_deferral_ms_ = clock_->now_ms();
    (void)save_state();
    return Status::error(ErrorCode::NotReady, decision.reason,
                         Json::object({{"rule", Json(decision.rule)}}));
  }

  const CampaignState previous = campaign_->state;
  campaign_->state = decision.next_state;
  campaign_->updated_at_ms = clock_->now_ms();
  campaign_->revision = Revision::from_value(campaign_->revision.value() + 1);
  campaign_->detail = decision.reason;
  campaign_->events.push_back(
      CampaignEvent{previous, decision.next_state, action, clock_->now_ms(), decision.reason});
  while (campaign_->events.size() > kMaxCampaignEvents) {
    campaign_->events.erase(campaign_->events.begin());
  }

  EpochEvent event;
  event.kind = EpochEventKind::CampaignTransition;
  event.shard = campaign_->shard;
  event.epoch = campaign_->epoch;
  event.generation = campaign_->generation;
  event.at_ms = clock_->now_ms();
  event.payload =
      Json::object({{"from", Json(std::string(::fabric::evolution::to_string(previous)))},
                    {"to", Json(std::string(::fabric::evolution::to_string(decision.next_state)))},
                    {"action", Json(std::string(::fabric::evolution::to_string(action)))},
                    {"rule", Json(decision.rule)}});
  const Status logged = epoch_log_.append(event);
  if (!logged.ok()) {
    return logged;
  }
  const Status saved = save_state();
  if (!saved.ok()) {
    return saved;
  }
  explanation.set_outcome(DecisionOutcome::Accepted);
  explanation.set_selected_action(std::string(::fabric::evolution::to_string(action)));
  explanation.set_resulting_state(std::string(::fabric::evolution::to_string(decision.next_state)));
  explanation.add_rejected_alternative(
      "refuse_" + std::string(::fabric::evolution::to_string(action)), decision.rule);
  return Status::success();
}

Status EvolutionController::bind_nodes() {
  if (!source_endpoint_.has_value() || !target_endpoint_.has_value()) {
    return Status::error(ErrorCode::NotFound, "manifest endpoints are not bound");
  }
  ServerOptions client_options;
  client_options.io_deadline_ms = options_.io_deadline_ms;
  NodeClient source(*source_endpoint_, client_options);
  NodeClient target(*target_endpoint_, client_options);
  auto source_report = source.report();
  if (!source_report.ok()) {
    return Status::error(source_report.status().code(),
                         "source component is unreachable: " + source_report.status().message());
  }
  auto target_report = target.report();
  if (!target_report.ok()) {
    return Status::error(target_report.status().code(),
                         "target component is unreachable: " + target_report.status().message());
  }
  last_source_report_ = source_report.value();
  last_target_report_ = target_report.value();
  return Status::success();
}

Status EvolutionController::ensure_predecessor_authority(Explanation& explanation) {
  if (!last_source_report_.has_value() || !source_endpoint_.has_value()) {
    return Status::error(ErrorCode::NotFound, "source component report is not available");
  }
  return ensure_holder_authority(*source_endpoint_, *last_source_report_, explanation);
}

Status EvolutionController::ensure_successor_authority(Explanation& explanation) {
  if (!last_target_report_.has_value() || !target_endpoint_.has_value()) {
    return Status::error(ErrorCode::NotFound, "target component report is not available");
  }
  return ensure_holder_authority(*target_endpoint_, *last_target_report_, explanation);
}

Status EvolutionController::ensure_holder_authority(const EndpointRef& endpoint,
                                                    const NodeReport& source,
                                                    Explanation& explanation) {
  if (!manifest_.has_value()) {
    return Status::error(ErrorCode::NotFound, "no manifest has been planned");
  }
  ServerOptions client_options;
  client_options.io_deadline_ms = options_.io_deadline_ms;
  NodeClient client(endpoint, client_options);
  const auto view = authority_.view(manifest_->shard);
  if (!view.ok()) {
    return view.status();
  }
  for (const FenceRecord& fence : view.value().fences) {
    if (!fence.acknowledged && fence.target == source.incarnation) {
      // A fence is outstanding for this incarnation. Granting it authority now
      // would contradict the fence, so the fence phase must complete first.
      return Status::success();
    }
  }
  if (view.value().mutating.has_value()) {
    const AuthorityLease& current = *view.value().mutating;
    if (current.holder == source.incarnation && source.has_authority && source.token == current.token) {
      const TimestampMs now_ms = clock_->now_ms();
      const TimestampMs renew_threshold =
          current.expires_at_ms > options_.lease_ttl_ms / 2
              ? current.expires_at_ms - options_.lease_ttl_ms / 2
              : current.expires_at_ms;
      if (current.attested && now_ms < renew_threshold) {
        return Status::success();
      }
      if (current.attested) {
        // Renew before the incumbent's lease runs out so that continuous service
        // is not interrupted by lease expiry during a long handoff.
        GrantRequest renew_request;
        renew_request.shard = manifest_->shard;
        renew_request.holder = current.holder;
        renew_request.mode = AuthorityMode::Mutating;
        renew_request.ttl_ms = options_.lease_ttl_ms;
        renew_request.campaign = manifest_->campaign;
        renew_request.reason = "lease renewed during the evolution campaign";
        auto renewed = authority_.grant(renew_request);
        if (!renewed.ok()) {
          return renewed.status();
        }
        Json body = Json::object();
        body.set("epoch", Json(renewed.value().epoch.value()));
        body.set("generation", Json(renewed.value().generation.value()));
        body.set("token", Json(renewed.value().token.to_compact_string()));
        body.set("mode", Json("mutating"));
        body.set("ttl_ms", Json(options_.lease_ttl_ms));
        body.set("handoff", Json(handoff_.has_value() ? handoff_->id.value() : 1));
        auto delivered = client.call("node.grant_authority", std::move(body));
        if (!delivered.ok()) {
          return delivered.status();
        }
        return save_state();
      }
      auto reattested = client.call(
          "node.reattest", Json::object({{"token", Json(current.token.to_compact_string())}}));
      if (reattested.ok()) {
        const Status attested = authority_.reattest(manifest_->shard, current.holder, current.token);
        if (!attested.ok()) {
          return attested;
        }
        return save_state();
      }
      explanation.add_note("re-attestation refused by the predecessor: " +
                           reattested.status().message());
    }
  }

  GrantRequest request;
  request.shard = manifest_->shard;
  request.holder = source.incarnation;
  request.mode = AuthorityMode::Mutating;
  request.ttl_ms = options_.lease_ttl_ms;
  request.campaign = manifest_->campaign;
  request.reason = "authority granted by the evolution controller";
  auto lease = authority_.grant(request);
  if (!lease.ok()) {
    explanation.add_note("initial authority grant refused: " + lease.status().message());
    return lease.status();
  }
  Json body = Json::object();
  body.set("epoch", Json(lease.value().epoch.value()));
  body.set("generation", Json(lease.value().generation.value()));
  body.set("token", Json(lease.value().token.to_compact_string()));
  body.set("mode", Json(std::string("mutating")));
  body.set("ttl_ms", Json(options_.lease_ttl_ms));
  body.set("handoff", Json(handoff_.has_value() ? handoff_->id.value() : 1));
  auto granted = client.call("node.grant_authority", std::move(body));
  if (!granted.ok()) {
    (void)authority_.revoke(manifest_->shard, lease.value().id, "grant delivery failed");
    explanation.add_note("initial authority delivery failed: " + granted.status().message());
    return granted.status();
  }
  EpochEvent event;
  event.kind = EpochEventKind::LeaseGranted;
  event.shard = manifest_->shard;
  event.epoch = lease.value().epoch;
  event.generation = lease.value().generation;
  event.at_ms = clock_->now_ms();
  event.payload = Json::object({{"holder", Json(lease.value().holder.to_string())},
                                {"reason", Json(lease.value().reason)}});
  (void)epoch_log_.append(event);
  return save_state();
}

Result<Json> EvolutionController::preflight_locked(Explanation& explanation) {
  explanation = Explanation("campaign.preflight", "fabric-evolution.preflight.v1");
  explanation.set_decided_at(clock_->now_ms());
  explanation.set_authority(decision_authority());
  if (!manifest_.has_value()) {
    explanation.set_outcome(DecisionOutcome::Rejected);
    return Status::error(ErrorCode::NotFound, "no manifest has been planned");
  }
  const Status transition = apply_action(CampaignAction::Preflight, explanation);
  if (!transition.ok()) {
    return transition;
  }

  const auto fail = [&](const Status& failure) -> Result<Json> {
    campaign_->state = CampaignState::Failed;
    campaign_->detail = failure.message();
    (void)save_state();
    explanation.set_outcome(DecisionOutcome::Rejected);
    explanation.set_selected_action("fail_preflight");
    explanation.add_rejected_alternative("start_campaign", failure.message());
    return failure;
  };

  const Status bound = bind_nodes();
  if (!bound.ok()) {
    return fail(bound);
  }

  ServerOptions client_options;
  client_options.io_deadline_ms = options_.io_deadline_ms;
  NodeClient source(*source_endpoint_, client_options);
  NodeClient target(*target_endpoint_, client_options);
  auto source_hello = source.hello();
  if (!source_hello.ok()) {
    return fail(source_hello.status());
  }
  auto target_hello = target.hello();
  if (!target_hello.ok()) {
    return fail(target_hello.status());
  }

  GroupNegotiationRequest negotiation;
  negotiation.participants.push_back(source_hello.value());
  negotiation.participants.push_back(target_hello.value());
  negotiation.required_participants.push_back(manifest_->source.id);
  negotiation.required_participants.push_back(manifest_->target.id);
  negotiation.path = manifest_->protocol;
  negotiation.allowed_schemas = {manifest_->source.schema, manifest_->target.schema};
  const NegotiatedProtocol negotiated = negotiate_group(negotiation);
  explanation.set_evidence(negotiated.to_json());
  if (!negotiated.usable()) {
    for (const std::string& refusal : negotiated.refusals) {
      explanation.add_rejected_alternative("start_campaign", refusal);
    }
    const ErrorCode code = negotiated.missing_required.empty() ? ErrorCode::CompatibilityInsufficient
                                                              : ErrorCode::FeatureNotNegotiated;
    return fail(Status::error(code, negotiated.rationale, negotiated.to_json()));
  }

  for (const MigrationStepSpec& step : manifest_->migrations) {
    auto resolved = resolve_migration_step(step);
    if (!resolved.ok()) {
      return fail(resolved.status());
    }
  }

  const Status authority_status = ensure_predecessor_authority(explanation);
  if (!authority_status.ok()) {
    return fail(authority_status);
  }

  const Status saved = save_state();
  if (!saved.ok()) {
    return saved;
  }
  explanation.set_outcome(DecisionOutcome::Accepted);
  explanation.set_selected_action("preflight_succeeded");
  explanation.set_resulting_state("preflighting");
  explanation.add_rejected_alternative("fail_preflight",
                                       "all compatibility, protocol and migration checks passed");
  return status_json_locked();
}

Status EvolutionController::record_phase(HandoffPhase to, std::string note) {
  if (!handoff_.has_value() || !campaign_.has_value() || !manifest_.has_value()) {
    return Status::error(ErrorCode::NotFound, "no handoff is in progress");
  }
  const Status valid = validate_handoff_transition(handoff_->phase, to);
  if (!valid.ok()) {
    return valid;
  }
  if (handoff_->phase == to) {
    return Status::success();
  }
  PhaseTransition transition;
  transition.from = handoff_->phase;
  transition.to = to;
  transition.at_ms = clock_->now_ms();
  transition.attempt = handoff_->attempt;
  transition.note = note;
  handoff_->history.push_back(transition);
  while (handoff_->history.size() > kMaxHandoffHistoryEntries) {
    handoff_->history.erase(handoff_->history.begin());
  }
  handoff_->phase = to;
  handoff_->updated_at_ms = clock_->now_ms();
  handoff_->epoch = authority_.current_epoch(handoff_->shard).value_or(handoff_->epoch);
  handoff_->generation = authority_.current_generation(handoff_->shard).value_or(handoff_->generation);
  if (campaign_.has_value()) {
    campaign_->epoch = handoff_->epoch;
    campaign_->generation = handoff_->generation;
  }
  EpochEvent event;
  event.kind = EpochEventKind::HandoffPhase;
  event.shard = handoff_->shard;
  event.epoch = handoff_->epoch;
  event.generation = handoff_->generation;
  event.at_ms = clock_->now_ms();
  event.payload =
      Json::object({{"handoff", Json(handoff_->id.value())},
                    {"from", Json(std::string(::fabric::evolution::to_string(transition.from)))},
                    {"to", Json(std::string(::fabric::evolution::to_string(to)))},
                    {"note", Json(note)}});
  const Status logged = epoch_log_.append(event);
  if (!logged.ok()) {
    return logged;
  }
  return save_state();
}

// A handoff is restarted when a participant process is replaced: the old
// incarnation is gone, so the prepared successor must be re-bound to the new
// source generation. The restart is explicit, recorded and bounded rather than
// silently re-entered. When the predecessor has already been fenced the handoff
// rewinds only to the snapshot phase, because a fenced predecessor is still a
// valid replication source - it simply may no longer mutate.
Status EvolutionController::restart_handoff(const IncarnationId& predecessor,
                                            const IncarnationId& successor, HandoffPhase rewind_to,
                                            std::string note) {
  if (!handoff_.has_value()) {
    return Status::error(ErrorCode::NotFound, "no handoff is in progress");
  }
  if (rewind_to != HandoffPhase::NotStarted && rewind_to != HandoffPhase::Prepared) {
    return invalid_argument("a handoff can only rewind to the start or to the snapshot phase");
  }
  if (handoff_->attempt.value() >= kMaxHandoffRestarts) {
    return Status::error(ErrorCode::ResourceExhausted,
                         "handoff has been restarted too many times after process restarts",
                         Json::object({{"attempts", Json(handoff_->attempt.value())}}));
  }
  const auto next_attempt = handoff_->attempt.next();
  if (!next_attempt.has_value()) {
    return Status::error(ErrorCode::BoundsExceeded, "handoff attempt counter is exhausted");
  }
  PhaseTransition transition;
  transition.from = handoff_->phase;
  transition.to = rewind_to;
  transition.at_ms = clock_->now_ms();
  transition.attempt = *next_attempt;
  transition.note = note;
  handoff_->history.push_back(transition);
  while (handoff_->history.size() > kMaxHandoffHistoryEntries) {
    handoff_->history.erase(handoff_->history.begin());
  }
  if (handoff_->predecessor != predecessor) {
    handoff_->previous_predecessor = handoff_->predecessor;
    predecessor_history_.push_back(handoff_->predecessor);
  }
  handoff_->predecessor = predecessor;
  handoff_->successor = successor;
  handoff_->attempt = *next_attempt;
  handoff_->phase = rewind_to;
  if (rewind_to == HandoffPhase::NotStarted) {
    handoff_->snapshot = SnapshotId::invalid();
    handoff_->snapshot_digest = Digest{};
    handoff_->snapshot_lsn = LogSequenceNumber::invalid();
    handoff_->target_lsn = LogSequenceNumber::invalid();
  }
  handoff_->updated_at_ms = clock_->now_ms();

  EpochEvent event;
  event.kind = EpochEventKind::Reconciliation;
  event.shard = handoff_->shard;
  event.epoch = handoff_->epoch;
  event.generation = handoff_->generation;
  event.at_ms = clock_->now_ms();
  event.payload = Json::object({{"handoff", Json(handoff_->id.value())},
                                {"reason", Json("participant incarnation replaced")},
                                {"note", Json(note)},
                                {"rewind_to", Json(std::string(::fabric::evolution::to_string(rewind_to)))},
                                {"predecessor", Json(predecessor.to_string())},
                                {"successor", Json(successor.to_string())}});
  const Status logged = epoch_log_.append(event);
  if (!logged.ok()) {
    return logged;
  }
  return save_state();
}

bool EvolutionController::incarnation_provably_replaced(const IncarnationId& recorded,
                                                        const IncarnationId& reported) {
  return recorded.is_valid() && reported.is_valid() &&
         recorded.component() == reported.component() && recorded != reported &&
         reported.boot() > recorded.boot();
}

std::optional<AuthorityClaim> EvolutionController::current_source_claim() const {
  if (!manifest_.has_value()) {
    return std::nullopt;
  }
  if (fenced_predecessor_claim_.has_value() &&
      fenced_predecessor_claim_->shard == manifest_->shard) {
    return fenced_predecessor_claim_;
  }
  const auto view = authority_.view(manifest_->shard);
  if (view.ok() && view.value().mutating.has_value()) {
    return claim_of(*view.value().mutating);
  }
  return std::nullopt;
}

Status EvolutionController::reconcile_incarnations(Explanation& explanation) {
  if (!manifest_.has_value() || !handoff_.has_value()) {
    return Status::success();
  }
  const auto view = authority_.view(manifest_->shard);
  if (!view.ok()) {
    return Status::success();
  }

  // A lease held by an incarnation the component has provably replaced is
  // revoked, together with any fence that was still waiting for it. This is what
  // lets a restarted participant rejoin without leaving the shard ownerless.
  if (view.value().slot_holder.has_value()) {
    const AuthorityLease& holder = *view.value().slot_holder;
    const NodeReport* report = nullptr;
    if (last_source_report_.has_value() &&
        last_source_report_->component == holder.holder.component()) {
      report = &*last_source_report_;
    } else if (last_target_report_.has_value() &&
               last_target_report_->component == holder.holder.component()) {
      report = &*last_target_report_;
    }
    if (report != nullptr && incarnation_provably_replaced(holder.holder, report->incarnation)) {
      for (const FenceRecord& fence : view.value().fences) {
        if (!fence.acknowledged && fence.target == holder.holder) {
          const Status acknowledged =
              authority_.acknowledge_fence(manifest_->shard, holder.holder, fence.id);
          if (!acknowledged.ok()) {
            return acknowledged;
          }
        }
      }
      const Status revoked = authority_.revoke(
          manifest_->shard, holder.id, "holder incarnation was provably replaced by a fresh boot");
      if (!revoked.ok()) {
        return revoked;
      }
      explanation.add_note("revoked the lease held by the replaced incarnation " +
                           holder.holder.to_string());
      EpochEvent event;
      event.kind = EpochEventKind::LeaseRevoked;
      event.shard = manifest_->shard;
      event.epoch = holder.epoch;
      event.generation = holder.generation;
      event.at_ms = clock_->now_ms();
      event.payload = Json::object({{"holder", Json(holder.holder.to_string())},
                                    {"reason", Json("incarnation provably replaced")}});
      const Status logged = epoch_log_.append(event);
      if (!logged.ok()) {
        return logged;
      }
    }
  }

  if (last_source_report_.has_value() &&
      handoff_->predecessor.component() == last_source_report_->component &&
      incarnation_provably_replaced(handoff_->predecessor, last_source_report_->incarnation)) {
    if (handoff_->phase < HandoffPhase::PredecessorFenced) {
      return restart_handoff(last_source_report_->incarnation, handoff_->successor,
                             HandoffPhase::NotStarted,
                             "predecessor incarnation was replaced before authority moved");
    }
  }
  if (last_target_report_.has_value() &&
      handoff_->successor.component() == last_target_report_->component &&
      incarnation_provably_replaced(handoff_->successor, last_target_report_->incarnation)) {
    if (handoff_->phase < HandoffPhase::AuthorityTransferred) {
      const HandoffPhase rewind = handoff_->phase >= HandoffPhase::PredecessorFenced
                                      ? HandoffPhase::Prepared
                                      : HandoffPhase::NotStarted;
      return restart_handoff(handoff_->predecessor, last_target_report_->incarnation, rewind,
                             "successor incarnation was replaced during the handoff");
    }
    handoff_->successor = last_target_report_->incarnation;
    explanation.add_note("handoff successor re-bound to the restarted incarnation");
    return save_state();
  }
  return Status::success();
}

Result<Json> EvolutionController::advance_prepared(Explanation& explanation) {
  ServerOptions client_options;
  client_options.io_deadline_ms = options_.io_deadline_ms;
  NodeClient source(*source_endpoint_, client_options);
  NodeClient target(*target_endpoint_, client_options);

  auto source_report = source.report();
  if (!source_report.ok()) {
    return source_report.status();
  }
  auto target_report = target.report();
  if (!target_report.ok()) {
    return target_report.status();
  }
  last_source_report_ = source_report.value();
  last_target_report_ = target_report.value();

  const Status authority_status = ensure_predecessor_authority(explanation);
  if (!authority_status.ok()) {
    return authority_status;
  }
  const auto source_claim = current_source_claim();
  if (!source_claim.has_value()) {
    return Status::error(ErrorCode::NotAuthoritative,
                         "predecessor holds no mutating authority to hand off");
  }
  const AuthorityClaim& source_lease = *source_claim;

  Json prepare_body = Json::object();
  prepare_body.set("handoff", Json(handoff_->id.value()));
  prepare_body.set("manifest_digest", Json(manifest_->digest.to_hex()));
  prepare_body.set("role", Json("predecessor"));
  auto prepared_source = source.call("node.prepare", prepare_body);
  if (!prepared_source.ok()) {
    return prepared_source.status();
  }

  Json successor_body = Json::object();
  successor_body.set("handoff", Json(handoff_->id.value()));
  successor_body.set("manifest_digest", Json(manifest_->digest.to_hex()));
  successor_body.set("role", Json("successor"));
  successor_body.set("target_software", Json(manifest_->target.software.to_string()));
  successor_body.set("target_protocol", Json(manifest_->target.protocol.to_string()));
  successor_body.set("target_schema", Json(manifest_->target.schema.value()));
  successor_body.set(
      "source", source_binding_json(source_lease, manifest_->source.schema, handoff_->id));
  auto prepared_target = target.call("node.prepare", std::move(successor_body));
  if (!prepared_target.ok()) {
    explanation.add_note("successor preparation was refused: " + prepared_target.status().message());
    return prepared_target.status();
  }

  handoff_->epoch = source_lease.epoch;
  handoff_->generation = source_lease.generation;
  return completed(record_phase(HandoffPhase::Prepared, "predecessor and successor prepared"));
}

Result<Json> EvolutionController::advance_snapshot(Explanation& explanation) {
  ServerOptions client_options;
  client_options.io_deadline_ms = options_.io_deadline_ms;
  NodeClient source(*source_endpoint_, client_options);
  NodeClient target(*target_endpoint_, client_options);

  // Preparation is idempotent. Re-issuing it makes the phase safe to replay and
  // lets a successor that restarted mid-handoff rejoin with a fresh binding.
  const auto source_claim = current_source_claim();
  if (!source_claim.has_value()) {
    return Status::error(ErrorCode::NotAuthoritative, "no source authority claim is recorded");
  }
  Json prepare_body = Json::object();
  prepare_body.set("handoff", Json(handoff_->id.value()));
  prepare_body.set("manifest_digest", Json(manifest_->digest.to_hex()));
  prepare_body.set("role", Json("successor"));
  prepare_body.set("target_software", Json(manifest_->target.software.to_string()));
  prepare_body.set("target_protocol", Json(manifest_->target.protocol.to_string()));
  prepare_body.set("target_schema", Json(manifest_->target.schema.value()));
  prepare_body.set(
      "source", source_binding_json(*source_claim, manifest_->source.schema, handoff_->id));
  auto prepared = target.call("node.prepare", std::move(prepare_body));
  if (!prepared.ok()) {
    explanation.add_note("successor preparation was refused: " + prepared.status().message());
    return prepared.status();
  }

  auto snapshot = source.call("node.snapshot", Json::object({{"handoff", Json(handoff_->id.value())}}));
  if (!snapshot.ok()) {
    explanation.add_note("snapshot request refused: " + snapshot.status().message());
    return snapshot.status();
  }
  const Json* snapshot_document = snapshot.value().find("snapshot");
  if (snapshot_document == nullptr) {
    return malformed("snapshot response is missing the snapshot document");
  }
  const auto snapshot_id = json_u64(*snapshot_document, "id");
  const auto snapshot_lsn = json_u64(*snapshot_document, "lsn");
  const auto snapshot_digest_text = json_string(*snapshot_document, "digest");
  if (!snapshot_id.has_value() || !snapshot_lsn.has_value() || !snapshot_digest_text.has_value()) {
    return malformed("snapshot header is incomplete");
  }
  auto digest = Digest::from_hex(*snapshot_digest_text);
  if (!digest.has_value()) {
    return malformed("snapshot digest is not a sha256 hex digest");
  }

  Json install_body = Json::object();
  install_body.set("handoff", Json(handoff_->id.value()));
  install_body.set("snapshot", *snapshot_document);
  auto installed = target.call("node.install_snapshot", std::move(install_body));
  if (!installed.ok()) {
    explanation.add_note("snapshot installation was refused: " + installed.status().message());
    return installed.status();
  }

  handoff_->snapshot = SnapshotId::from_value(*snapshot_id);
  handoff_->snapshot_digest = *digest;
  handoff_->snapshot_lsn = LogSequenceNumber::from_value(*snapshot_lsn);
  handoff_->target_lsn = LogSequenceNumber::from_value(*snapshot_lsn);
  return completed(record_phase(HandoffPhase::SnapshotSynchronized,
                                "snapshot transferred and verified by the successor"));
}

Result<Json> EvolutionController::advance_catch_up(Explanation& explanation) {
  ServerOptions client_options;
  client_options.io_deadline_ms = options_.io_deadline_ms;
  NodeClient source(*source_endpoint_, client_options);
  NodeClient target(*target_endpoint_, client_options);

  std::size_t rounds = 0;
  std::size_t snapshot_refreshes = 0;
  std::uint64_t target_lsn = handoff_->target_lsn.value();
  while (rounds < options_.max_catch_up_rounds) {
    ++rounds;
    Json read_body = Json::object();
    read_body.set("from_lsn", Json(target_lsn));
    read_body.set("max_records", Json(static_cast<std::uint64_t>(options_.max_records_per_round)));
    auto records = source.call("node.read_since", std::move(read_body));
    if (!records.ok()) {
      return records.status();
    }
    if (json_bool_or(records.value(), "truncated", false)) {
      if (snapshot_refreshes >= 2) {
        return Status::error(ErrorCode::MigrationFailure,
                             "incremental catch-up cannot proceed and the snapshot keeps being "
                             "truncated by the predecessor log bound");
      }
      ++snapshot_refreshes;
      auto refreshed = advance_snapshot(explanation);
      if (!refreshed.ok()) {
        return refreshed.status();
      }
      target_lsn = handoff_->target_lsn.value();
      continue;
    }
    const Json* entries = records.value().find("records");
    if (entries == nullptr || !entries->is_array()) {
      return malformed("read_since response has no records array");
    }
    if (entries->size() == 0) {
      break;
    }
    const auto source_lsn_value = json_u64_or(records.value(), "lsn", 0);
    if (source_lsn_value <= target_lsn) {
      break;
    }
    const auto source_claim = current_source_claim();
    if (!source_claim.has_value()) {
      return Status::error(ErrorCode::NotAuthoritative, "no source authority claim is recorded");
    }
    Json apply_body = Json::object();
    apply_body.set("handoff", Json(handoff_->id.value()));
    apply_body.set("source",
                   source_binding_json(*source_claim, manifest_->source.schema, handoff_->id));
    apply_body.set("records", *entries);
    auto applied = target.call("node.apply_records", std::move(apply_body));
    if (!applied.ok()) {
      explanation.add_note("catch-up application refused: " + applied.status().message());
      return applied.status();
    }
    const auto applied_lsn = json_u64_or(applied.value(), "lsn", target_lsn);
    if (applied_lsn <= target_lsn) {
      break;
    }
    target_lsn = applied_lsn;
    handoff_->target_lsn = LogSequenceNumber::from_value(target_lsn);
  }

  auto source_report = source.report();
  if (!source_report.ok()) {
    return source_report.status();
  }
  auto target_report = target.report();
  if (!target_report.ok()) {
    return target_report.status();
  }
  last_source_report_ = source_report.value();
  last_target_report_ = target_report.value();
  if (target_report.value().lsn < source_report.value().lsn) {
    return Status::error(
        ErrorCode::NotReady, "successor has not caught up with the predecessor's log position",
        Json::object({{"predecessor_lsn", Json(source_report.value().lsn.value())},
                      {"successor_lsn", Json(target_report.value().lsn.value())}}));
  }
  handoff_->target_lsn = source_report.value().lsn;

  if (target_report.value().schema != manifest_->target.schema) {
    Json steps = Json::array();
    for (const MigrationStepSpec& step : manifest_->migrations) {
      steps.push_back(::fabric::evolution::to_json(step));
    }
    Json migrate_body = Json::object();
    migrate_body.set("handoff", Json(handoff_->id.value()));
    migrate_body.set("steps", std::move(steps));
    auto migrated = target.call("node.migrate", std::move(migrate_body));
    if (!migrated.ok()) {
      explanation.add_note("state migration on the successor was refused: " +
                           migrated.status().message());
      return migrated.status();
    }
    const auto crossed = json_bool_or(migrated.value(), "crossed_irreversible_boundary", false);
    if (crossed) {
      campaign_->irreversible_boundary_crossed = true;
      for (const MigrationStepSpec& step : manifest_->migrations) {
        if (step.irreversible_boundary) {
          if (std::find(campaign_->crossed_boundaries.begin(), campaign_->crossed_boundaries.end(),
                        step.id) == campaign_->crossed_boundaries.end()) {
            campaign_->crossed_boundaries.push_back(step.id);
          }
        }
      }
      EpochEvent event;
      event.kind = EpochEventKind::MigrationApplied;
      event.shard = manifest_->shard;
      event.epoch = handoff_->epoch;
      event.generation = handoff_->generation;
      event.at_ms = clock_->now_ms();
      event.payload = Json::object(
          {{"state_digest_after", Json(json_string_or(migrated.value(), "state_digest_after", ""))},
           {"irreversible", Json(true)}});
      const Status logged = epoch_log_.append(event);
      if (!logged.ok()) {
        return logged;
      }
    }
  }
  return completed(record_phase(HandoffPhase::CaughtUp,
                                "successor caught up and migrated to the target schema"));
}

Result<Json> EvolutionController::advance_readiness(Explanation& explanation) {
  ServerOptions client_options;
  client_options.io_deadline_ms = options_.io_deadline_ms;
  NodeClient target(*target_endpoint_, client_options);
  Json body = Json::object();
  body.set("handoff", Json(handoff_->id.value()));
  body.set("target_lsn", Json(handoff_->target_lsn.value()));
  body.set("target_schema", Json(manifest_->target.schema.value()));
  auto ready = target.call("node.readiness", std::move(body));
  if (!ready.ok()) {
    return ready.status();
  }
  if (!json_bool_or(ready.value(), "ready", false)) {
    const std::string reason = json_string_or(ready.value(), "reason", "successor is not ready");
    explanation.add_note(reason);
    return Status::error(ErrorCode::NotReady, reason);
  }
  handoff_->target_lsn = LogSequenceNumber::from_value(json_u64_or(ready.value(), "lsn",
                                                                   handoff_->target_lsn.value()));
  return completed(record_phase(HandoffPhase::ReadinessVerified, "successor reported ready"));
}

Result<Json> EvolutionController::advance_fence(Explanation& explanation) {
  ServerOptions client_options;
  client_options.io_deadline_ms = options_.io_deadline_ms;
  NodeClient source(*source_endpoint_, client_options);

  auto view = authority_.view(manifest_->shard);
  if (!view.ok()) {
    return view.status();
  }
  if (!view.value().mutating.has_value() && last_source_report_.has_value() &&
      (last_source_report_->lifecycle == NodeLifecycle::Fenced ||
       last_source_report_->lifecycle == NodeLifecycle::Retired)) {
    // The predecessor reports that it is already fenced. That report is itself the
    // acknowledgement of any fence still outstanding in the registry; recording it
    // is what releases the mutation slot for the successor.
    for (const FenceRecord& fence : view.value().fences) {
      if (!fence.acknowledged && fence.target == handoff_->predecessor) {
        const Status acknowledged =
            authority_.acknowledge_fence(manifest_->shard, handoff_->predecessor, fence.id);
        if (!acknowledged.ok()) {
          return acknowledged;
        }
        EpochEvent event;
        event.kind = EpochEventKind::FenceAcknowledged;
        event.shard = manifest_->shard;
        event.epoch = fence.epoch;
        event.generation = fence.generation;
        event.at_ms = clock_->now_ms();
        event.payload = Json::object({{"fence_id", Json(fence.id.value())},
                                      {"holder", Json(handoff_->predecessor.to_string())},
                                      {"source", Json("component report")}});
        const Status logged = epoch_log_.append(event);
        if (!logged.ok()) {
          return logged;
        }
      }
    }
    return completed(record_phase(HandoffPhase::PredecessorFenced,
                                  "predecessor reports it is already fenced"));
  }
  std::optional<FenceRecord> outstanding;
  for (auto it = view.value().fences.rbegin(); it != view.value().fences.rend(); ++it) {
    if (!it->acknowledged && it->target == handoff_->predecessor) {
      outstanding = *it;
      break;
    }
  }
  if (!outstanding.has_value()) {
    const Status fence_request =
        authority_.request_fence(manifest_->shard, handoff_->predecessor,
                                 "handoff phase predecessor fence");
    if (!fence_request.ok() && fence_request.code() != ErrorCode::DuplicateFrame) {
      return fence_request;
    }
    const Status saved = save_state();
    if (!saved.ok()) {
      return saved;
    }
    view = authority_.view(manifest_->shard);
    if (!view.ok()) {
      return view.status();
    }
    for (auto it = view.value().fences.rbegin(); it != view.value().fences.rend(); ++it) {
      if (!it->acknowledged && it->target == handoff_->predecessor) {
        outstanding = *it;
        break;
      }
    }
    if (!outstanding.has_value()) {
      return Status::error(ErrorCode::Internal, "fence was requested but no outstanding fence exists");
    }
  }

  Json body = Json::object();
  body.set("handoff", Json(handoff_->id.value()));
  body.set("fence_id", Json(outstanding->id.value()));
  body.set("target", ::fabric::evolution::to_json(handoff_->predecessor));
  auto acknowledged = source.call("node.fence", std::move(body));
  if (!acknowledged.ok()) {
    // The fence stays outstanding: authority is not transferred while the
    // predecessor has neither acknowledged nor provably died.
    last_deferral_reason_ = "predecessor has not acknowledged the fence: " +
                            acknowledged.status().message();
    (void)save_state();
    explanation.add_note(last_deferral_reason_);
    explanation.set_outcome(DecisionOutcome::Deferred);
    return Status::error(ErrorCode::NotReady, last_deferral_reason_,
                         Json::object({{"fence_id", Json(outstanding->id.value())}}));
  }
  const Status fence_ack = authority_.acknowledge_fence(manifest_->shard, handoff_->predecessor,
                                                        outstanding->id);
  if (!fence_ack.ok()) {
    return fence_ack;
  }
  handoff_->fence = outstanding->id;
  handoff_->epoch = outstanding->epoch;
  handoff_->generation = outstanding->generation;
  EpochEvent event;
  event.kind = EpochEventKind::FenceAcknowledged;
  event.shard = manifest_->shard;
  event.epoch = handoff_->epoch;
  event.generation = handoff_->generation;
  event.at_ms = clock_->now_ms();
  event.payload = Json::object({{"fence_id", Json(outstanding->id.value())},
                                {"holder", Json(handoff_->predecessor.to_string())}});
  const Status logged = epoch_log_.append(event);
  if (!logged.ok()) {
    return logged;
  }
  return completed(record_phase(HandoffPhase::PredecessorFenced, "predecessor acknowledged the fence"));
}

Result<Json> EvolutionController::advance_transfer(Explanation& explanation) {
  ServerOptions client_options;
  client_options.io_deadline_ms = options_.io_deadline_ms;
  NodeClient source(*source_endpoint_, client_options);
  NodeClient target(*target_endpoint_, client_options);

  // Final catch-up: the predecessor is fenced, so its log position is now stable.
  auto source_report = source.report();
  if (!source_report.ok()) {
    return source_report.status();
  }
  std::size_t rounds = 0;
  while (rounds < options_.max_catch_up_rounds) {
    ++rounds;
    auto target_report = target.report();
    if (!target_report.ok()) {
      return target_report.status();
    }
    if (target_report.value().lsn >= source_report.value().lsn) {
      break;
    }
    Json read_body = Json::object();
    read_body.set("from_lsn", Json(target_report.value().lsn.value()));
    read_body.set("max_records", Json(static_cast<std::uint64_t>(options_.max_records_per_round)));
    auto records = source.call("node.read_since", std::move(read_body));
    if (!records.ok()) {
      return records.status();
    }
    const Json* entries = records.value().find("records");
    if (entries == nullptr || !entries->is_array() || entries->size() == 0) {
      break;
    }
    const auto source_claim = current_source_claim();
    if (!source_claim.has_value()) {
      return Status::error(ErrorCode::NotAuthoritative, "no source authority claim is recorded");
    }
    Json apply_body = Json::object();
    apply_body.set("handoff", Json(handoff_->id.value()));
    apply_body.set("source",
                   source_binding_json(*source_claim, manifest_->source.schema, handoff_->id));
    apply_body.set("records", *entries);
    auto applied = target.call("node.apply_records", std::move(apply_body));
    if (!applied.ok()) {
      return applied.status();
    }
  }
  handoff_->target_lsn = source_report.value().lsn;
  last_source_report_ = source_report.value();

  GrantRequest request;
  request.shard = manifest_->shard;
  request.holder = handoff_->successor;
  request.mode = AuthorityMode::Mutating;
  request.ttl_ms = options_.lease_ttl_ms;
  request.campaign = manifest_->campaign;
  request.handoff = handoff_->id;
  request.reason = "authority transferred to the successor";
  auto lease = authority_.grant(request);
  if (!lease.ok()) {
    explanation.add_note("authority transfer was refused: " + lease.status().message());
    return lease.status();
  }

  Json body = Json::object();
  body.set("handoff", Json(handoff_->id.value()));
  body.set("epoch", Json(lease.value().epoch.value()));
  body.set("generation", Json(lease.value().generation.value()));
  body.set("token", Json(lease.value().token.to_compact_string()));
  body.set("mode", Json("mutating"));
  body.set("ttl_ms", Json(options_.lease_ttl_ms));
  auto granted = target.call("node.grant_authority", std::move(body));
  if (!granted.ok()) {
    (void)authority_.revoke(manifest_->shard, lease.value().id, "grant delivery failed");
    explanation.add_note("successor did not accept authority: " + granted.status().message());
    (void)save_state();
    return granted.status();
  }
  handoff_->token = lease.value().token;
  handoff_->epoch = lease.value().epoch;
  handoff_->generation = lease.value().generation;

  EpochEvent event;
  event.kind = EpochEventKind::LeaseGranted;
  event.shard = manifest_->shard;
  event.epoch = lease.value().epoch;
  event.generation = lease.value().generation;
  event.at_ms = clock_->now_ms();
  event.payload = Json::object({{"holder", Json(lease.value().holder.to_string())},
                                {"reason", Json(lease.value().reason)}});
  const Status logged = epoch_log_.append(event);
  if (!logged.ok()) {
    return logged;
  }
  return completed(record_phase(HandoffPhase::AuthorityTransferred,
                                "mutating authority granted to the successor"));
}

Result<Json> EvolutionController::advance_verify(Explanation& explanation) {
  ServerOptions client_options;
  client_options.io_deadline_ms = options_.io_deadline_ms;
  NodeClient source(*source_endpoint_, client_options);
  NodeClient target(*target_endpoint_, client_options);

  const auto view = authority_.view(manifest_->shard);
  if (!view.ok() || !view.value().mutating.has_value()) {
    return Status::error(ErrorCode::NotAuthoritative, "no mutating authority is recorded");
  }
  const AuthorityLease& lease = *view.value().mutating;

  auto target_report = target.report();
  if (!target_report.ok()) {
    return target_report.status();
  }
  if (!target_report.value().has_authority || target_report.value().token != lease.token ||
      target_report.value().generation != lease.generation ||
      target_report.value().epoch != lease.epoch) {
    explanation.add_note("successor does not attest the transferred authority");
    return Status::error(
        ErrorCode::NotReady, "successor does not attest the transferred authority",
        Json::object({{"successor_generation", Json(target_report.value().generation.value())},
                      {"expected_generation", Json(lease.generation.value())},
                      {"successor_authoritative", Json(target_report.value().has_authority)}}));
  }

  // Active proof that the fenced predecessor can no longer mutate: present its
  // own pre-fence credentials and require a refusal.
  if (fenced_predecessor_claim_.has_value()) {
    Json probe = Json::object();
    probe.set("claim", ::fabric::evolution::to_json(*fenced_predecessor_claim_));
    probe.set("key", Json("__evolution_probe__"));
    probe.set("value", Json("stale-write-attempt"));
    auto stale_write = source.call("node.write", std::move(probe));
    if (stale_write.ok()) {
      explanation.set_outcome(DecisionOutcome::Rejected);
      explanation.add_rejected_alternative(
          "accept_successor", "the fenced predecessor accepted a mutating write");
      return Status::error(
          ErrorCode::AuthorityConflict,
          "the fenced predecessor accepted a mutating write; refusing to declare the handoff safe",
          Json::object({{"predecessor", Json(handoff_->predecessor.to_string())}}));
    }
    explanation.add_note("fenced predecessor refused a stale write with code " +
                         std::string(::fabric::evolution::to_string(stale_write.status().code())));
  }

  // Active proof that the successor can mutate under the new authority.
  Json successor_probe = Json::object();
  AuthorityClaim claim = claim_of(lease);
  successor_probe.set("claim", ::fabric::evolution::to_json(claim));
  successor_probe.set("key", Json("__evolution_probe__"));
  successor_probe.set("value", Json(std::to_string(lease.generation.value())));
  auto live_write = target.call("node.write", std::move(successor_probe));
  if (!live_write.ok()) {
    explanation.add_note("successor refused a write under the new authority: " +
                         live_write.status().message());
    return live_write.status();
  }
  return completed(record_phase(HandoffPhase::SuccessorVerified,
                                "successor attested authority and the predecessor was proven fenced"));
}

Result<Json> EvolutionController::advance_retire(Explanation& explanation) {
  ServerOptions client_options;
  client_options.io_deadline_ms = options_.io_deadline_ms;
  NodeClient source(*source_endpoint_, client_options);
  auto retired = source.call("node.retire", Json::object({{"handoff", Json(handoff_->id.value())}}));
  if (!retired.ok()) {
    explanation.add_note("predecessor retirement failed: " + retired.status().message());
    return retired.status();
  }
  const Status phased = record_phase(HandoffPhase::PredecessorRetired,
                                     "predecessor retired; successor is authoritative");
  if (!phased.ok()) {
    return phased;
  }
  const Status finished = apply_action(CampaignAction::Complete, explanation);
  if (!finished.ok()) {
    return finished;
  }
  return status_json_locked();
}

Result<Json> EvolutionController::advance_locked(Explanation& explanation) {
  explanation = Explanation("handoff.advance", "fabric-evolution.handoff.v1");
  explanation.set_decided_at(clock_->now_ms());
  explanation.set_authority(decision_authority());
  if (!manifest_.has_value() || !campaign_.has_value()) {
    explanation.set_outcome(DecisionOutcome::Rejected);
    return Status::error(ErrorCode::NotFound, "no campaign has been planned");
  }
  if (campaign_->state == CampaignState::Paused) {
    explanation.set_outcome(DecisionOutcome::Deferred);
    return Status::error(ErrorCode::NotReady, "the campaign is paused");
  }
  if (is_terminal_state(campaign_->state)) {
    return status_json_locked();
  }
  if (!handoff_.has_value()) {
    // advance performs exactly one phase: creating the handoff is not a phase, so
    // the NotStarted phase still runs below in this same call.
    const Status begun = begin_handoff_locked(explanation);
    if (!begun.ok()) {
      return begun;
    }
  }
  if (!source_endpoint_.has_value() || !target_endpoint_.has_value()) {
    return Status::error(ErrorCode::NotFound, "manifest endpoints are not bound");
  }

  const Status refreshed = bind_nodes();
  if (!refreshed.ok()) {
    explanation.set_outcome(DecisionOutcome::Deferred);
    explanation.add_note(refreshed.message());
    return refreshed;
  }
  const Status reconciled = reconcile_incarnations(explanation);
  if (!reconciled.ok()) {
    explanation.set_outcome(DecisionOutcome::Rejected);
    explanation.add_note("incarnation reconciliation failed: " + reconciled.message());
    return reconciled;
  }
  if (is_forward_phase(handoff_->phase) && handoff_->phase < HandoffPhase::PredecessorFenced) {
    const Status renewed = ensure_predecessor_authority(explanation);
    if (!renewed.ok()) {
      explanation.set_outcome(DecisionOutcome::Deferred);
      explanation.add_note("incumbent authority could not be renewed: " + renewed.message());
      return renewed;
    }
  } else if (handoff_->phase >= HandoffPhase::AuthorityTransferred) {
    const Status renewed = ensure_successor_authority(explanation);
    if (!renewed.ok()) {
      explanation.set_outcome(DecisionOutcome::Deferred);
      explanation.add_note("successor authority could not be renewed: " + renewed.message());
      return renewed;
    }
  }

  const WindowStatus window = window_status_locked();
  if (window.exhausted) {
    const Status paused = apply_action(CampaignAction::Pause, explanation);
    (void)paused;
    explanation.set_outcome(DecisionOutcome::Deferred);
    explanation.add_note(window.reason);
    return Status::error(ErrorCode::NotReady, window.reason,
                         Json::object({{"operations", Json(window.operations)},
                                       {"operation_limit", Json(window.operation_limit)},
                                       {"elapsed_ms", Json(window.elapsed_ms)},
                                       {"duration_limit_ms", Json(window.duration_limit_ms)}}));
  }

  switch (handoff_->phase) {
    case HandoffPhase::NotStarted:
      return advance_prepared(explanation);
    case HandoffPhase::Prepared:
      return advance_snapshot(explanation);
    case HandoffPhase::SnapshotSynchronized:
      return advance_catch_up(explanation);
    case HandoffPhase::CaughtUp:
      return advance_readiness(explanation);
    case HandoffPhase::ReadinessVerified:
      return advance_fence(explanation);
    case HandoffPhase::PredecessorFenced:
      return advance_transfer(explanation);
    case HandoffPhase::AuthorityTransferred:
      return advance_verify(explanation);
    case HandoffPhase::SuccessorVerified:
      return advance_retire(explanation);
    case HandoffPhase::PredecessorRetired:
      return status_json_locked();
    case HandoffPhase::Failed:
    case HandoffPhase::Aborted:
      explanation.set_outcome(DecisionOutcome::Rejected);
      return Status::error(ErrorCode::IllegalTransition, "the handoff has already terminated");
  }
  return Status::error(ErrorCode::Internal, "unreachable handoff phase");
}

Result<Json> EvolutionController::run_to_completion_locked(Explanation& explanation) {
  const std::size_t max_steps = 64;
  if (!campaign_.has_value()) {
    return Status::error(ErrorCode::NotFound, "no campaign has been planned");
  }
  if (!handoff_.has_value()) {
    // The campaign has been preflighted but the handoff has not been created yet.
    return start_locked(explanation);
  }
  for (std::size_t step = 0; step < max_steps; ++step) {
    if (!campaign_.has_value() || !handoff_.has_value()) {
      return Status::error(ErrorCode::NotFound, "no campaign is in progress");
    }
    if (campaign_->state == CampaignState::Paused) {
      explanation.set_outcome(DecisionOutcome::Deferred);
      explanation.add_note("the campaign paused at a phase boundary");
      return status_json_locked();
    }
    if (is_terminal_state(campaign_->state)) {
      return status_json_locked();
    }
    Explanation step_explanation("handoff.advance", "fabric-evolution.handoff.v1");
    step_explanation.set_decided_at(clock_->now_ms());
    step_explanation.set_authority(decision_authority());
    auto advanced = advance_locked(step_explanation);
    if (!advanced.ok()) {
      explanation = std::move(step_explanation);
      return advanced.status();
    }
    if (handoff_->phase == HandoffPhase::PredecessorRetired) {
      explanation = std::move(step_explanation);
      return status_json_locked();
    }
  }
  return Status::error(ErrorCode::ResourceExhausted,
                       "handoff did not reach retirement within the permitted number of phases");
}

Status EvolutionController::begin_handoff_locked(Explanation& explanation) {
  if (!manifest_.has_value() || !campaign_.has_value()) {
    return Status::error(ErrorCode::NotFound, "no campaign has been planned");
  }
  if (handoff_.has_value()) {
    return Status::success();
  }
  const Status transition = apply_action(CampaignAction::Start, explanation);
  if (!transition.ok()) {
    return transition;
  }
  const Status bound = bind_nodes();
  if (!bound.ok()) {
    return bound;
  }
  {
    HandoffRecord record;
    record.id = HandoffId::from_value(next_handoff_id_);
    next_handoff_id_ += 1;
    record.campaign = manifest_->campaign;
    record.shard = manifest_->shard;
    record.manifest = manifest_->id;
    record.manifest_digest = manifest_->digest;
    record.predecessor = last_source_report_->incarnation;
    record.successor = last_target_report_->incarnation;
    record.phase = HandoffPhase::NotStarted;
    record.attempt = AttemptNumber::from_value(1);
    record.epoch = authority_.current_epoch(manifest_->shard).value_or(EpochNumber::invalid());
    record.generation =
        authority_.current_generation(manifest_->shard).value_or(Generation::invalid());
    record.started_at_ms = clock_->now_ms();
    record.updated_at_ms = record.started_at_ms;
    handoff_ = record;
    campaign_->handoff = record.id;
  }
  const Status authority_status = ensure_predecessor_authority(explanation);
  if (!authority_status.ok()) {
    return authority_status;
  }
  const auto view = authority_.view(manifest_->shard);
  if (view.ok() && view.value().mutating.has_value()) {
    const AuthorityLease& lease = *view.value().mutating;
    if (lease.holder == handoff_->predecessor && !fenced_predecessor_claim_.has_value()) {
      fenced_predecessor_claim_ = claim_of(lease);
    }
  }
  if (campaign_->window_started_at_ms == 0) {
    campaign_->window_started_at_ms = clock_->now_ms();
    window_start_lsn_ = last_source_report_->lsn;
    EpochEvent event;
    event.kind = EpochEventKind::Reconciliation;
    event.shard = manifest_->shard;
    event.epoch = handoff_->epoch;
    event.generation = handoff_->generation;
    event.at_ms = clock_->now_ms();
    event.payload = Json::object({{"window", Json("opened")},
                                  {"start_lsn", Json(window_start_lsn_.value())}});
    const Status logged = epoch_log_.append(event);
    if (!logged.ok()) {
      return logged;
    }
  }
  return save_state();
}

Result<Json> EvolutionController::start_locked(Explanation& explanation) {
  explanation = Explanation("campaign.start", "fabric-evolution.handoff.v1");
  explanation.set_decided_at(clock_->now_ms());
  explanation.set_authority(decision_authority());
  const Status begun = begin_handoff_locked(explanation);
  if (!begun.ok()) {
    return begun;
  }
  return run_to_completion_locked(explanation);
}

Result<Json> EvolutionController::pause_locked(Explanation& explanation) {
  explanation = Explanation("campaign.pause", "fabric-evolution.campaign.v1");
  explanation.set_decided_at(clock_->now_ms());
  explanation.set_authority(decision_authority());
  const Status transition = apply_action(CampaignAction::Pause, explanation);
  if (!transition.ok()) {
    return transition;
  }
  return status_json_locked();
}

Result<Json> EvolutionController::resume_locked(Explanation& explanation) {
  explanation = Explanation("campaign.resume", "fabric-evolution.campaign.v1");
  explanation.set_decided_at(clock_->now_ms());
  explanation.set_authority(decision_authority());
  const Status transition = apply_action(CampaignAction::Resume, explanation);
  if (!transition.ok()) {
    return transition;
  }
  return run_to_completion_locked(explanation);
}

Result<Json> EvolutionController::abort_locked(Explanation& explanation) {
  explanation = Explanation("campaign.abort", "fabric-evolution.campaign.v1");
  explanation.set_decided_at(clock_->now_ms());
  explanation.set_authority(decision_authority());
  if (!manifest_.has_value() || !campaign_.has_value()) {
    return Status::error(ErrorCode::NotFound, "no campaign has been planned");
  }
  const Status transition = apply_action(CampaignAction::Abort, explanation);
  if (!transition.ok()) {
    return transition;
  }
  if (campaign_->state == CampaignState::Aborted) {
    return status_json_locked();
  }
  if (campaign_->state == CampaignState::ForwardRecovery) {
    explanation.add_note("rollback is unsafe; the campaign continues forward");
    return run_to_completion_locked(explanation);
  }
  // Rollback: the predecessor keeps (or regains) authority and the successor is
  // retired. This is only reachable while no irreversible boundary was crossed.
  ServerOptions client_options;
  client_options.io_deadline_ms = options_.io_deadline_ms;
  if (source_endpoint_.has_value() && handoff_.has_value()) {
    NodeClient source(*source_endpoint_, client_options);
    auto source_report = source.report();
    if (source_report.ok()) {
      const auto view = authority_.view(manifest_->shard);
      const bool predecessor_authoritative =
          view.ok() && view.value().mutating.has_value() &&
          view.value().mutating->holder == source_report.value().incarnation;
      if (!predecessor_authoritative) {
        GrantRequest request;
        request.shard = manifest_->shard;
        request.holder = source_report.value().incarnation;
        request.mode = AuthorityMode::Mutating;
        request.ttl_ms = options_.lease_ttl_ms;
        request.campaign = manifest_->campaign;
        request.reason = "authority restored to the predecessor after a reversible abort";
        auto lease = authority_.grant(request);
        if (lease.ok()) {
          Json body = Json::object();
          body.set("handoff", Json(handoff_->id.value()));
          body.set("epoch", Json(lease.value().epoch.value()));
          body.set("generation", Json(lease.value().generation.value()));
          body.set("token", Json(lease.value().token.to_compact_string()));
          body.set("mode", Json("mutating"));
          body.set("ttl_ms", Json(options_.lease_ttl_ms));
          auto granted = source.call("node.grant_authority", std::move(body));
          if (!granted.ok()) {
            explanation.add_note("could not restore predecessor authority: " +
                                 granted.status().message());
          }
        } else {
          explanation.add_note("could not restore predecessor authority: " + lease.status().message());
        }
      }
    } else {
      explanation.add_note("predecessor is unreachable during rollback: " +
                           source_report.status().message());
    }
  }
  if (target_endpoint_.has_value() && handoff_.has_value()) {
    NodeClient target(*target_endpoint_, client_options);
    auto retired = target.call("node.retire",
                               Json::object({{"handoff", Json(handoff_->id.value())}}));
    if (!retired.ok()) {
      explanation.add_note("successor retirement failed: " + retired.status().message());
    }
  }
  if (handoff_.has_value()) {
    handoff_->phase = HandoffPhase::Aborted;
    handoff_->updated_at_ms = clock_->now_ms();
    handoff_->last_error = "campaign aborted before authority moved";
  }
  const Status confirmed = apply_action(CampaignAction::ConfirmAbort, explanation);
  if (!confirmed.ok()) {
    return confirmed;
  }
  const Status saved = save_state();
  if (!saved.ok()) {
    return saved;
  }
  return status_json_locked();
}

Result<Json> EvolutionController::reconcile_locked(Explanation& explanation) {
  explanation = Explanation("controller.reconcile", "fabric-evolution.reconciliation.v1");
  explanation.set_decided_at(clock_->now_ms());
  explanation.set_authority(decision_authority());
  if (!manifest_.has_value()) {
    explanation.set_outcome(DecisionOutcome::Rejected);
    return Status::error(ErrorCode::NotFound, "no manifest has been planned");
  }
  const Status bound = bind_nodes();
  if (!bound.ok()) {
    explanation.set_outcome(DecisionOutcome::Rejected);
    explanation.set_selected_action("defer_reconciliation");
    explanation.add_rejected_alternative("reconcile", bound.message());
    return bound;
  }
  Json inputs = Json::object();
  inputs.set("source_report", last_source_report_->to_json());
  inputs.set("target_report", last_target_report_->to_json());
  inputs.set("handoff_phase", Json(std::string(::fabric::evolution::to_string(
                                   handoff_.has_value() ? handoff_->phase : HandoffPhase::NotStarted))));
  explanation.set_inputs(std::move(inputs));

  const Status incarnation_status = reconcile_incarnations(explanation);
  if (!incarnation_status.ok()) {
    explanation.set_outcome(DecisionOutcome::Rejected);
    explanation.add_note("incarnation reconciliation failed: " + incarnation_status.message());
    return incarnation_status;
  }
  // A predecessor that has already been fenced must never be re-granted
  // authority: reconciliation restores authority only before the fence.
  if (!handoff_.has_value() || handoff_->phase < HandoffPhase::PredecessorFenced) {
    const Status authority_status = ensure_predecessor_authority(explanation);
    if (!authority_status.ok()) {
      explanation.add_note("authority reconciliation: " + authority_status.message());
    }
  }
  if (handoff_.has_value() && handoff_->phase >= HandoffPhase::AuthorityTransferred) {
    const Status successor_status = ensure_successor_authority(explanation);
    if (!successor_status.ok()) {
      explanation.add_note("successor authority reconciliation: " + successor_status.message());
    }
  }

  if (!handoff_.has_value()) {
    explanation.set_outcome(DecisionOutcome::Accepted);
    explanation.set_selected_action("no_handoff_to_reconcile");
    return status_json_locked();
  }

  const auto view = authority_.view(manifest_->shard);
  if (view.ok() && view.value().mutating.has_value()) {
    const AuthorityLease& lease = *view.value().mutating;
    const NodeReport& target = *last_target_report_;
    if (lease.holder == handoff_->successor && target.has_authority && target.token == lease.token &&
        handoff_->phase < HandoffPhase::AuthorityTransferred) {
      const Status phased = record_phase(
          HandoffPhase::AuthorityTransferred,
          "reconciled: the successor reported the transferred authority after a controller restart");
      if (!phased.ok()) {
        return phased;
      }
      explanation.add_note("handoff phase advanced from component-reported authority");
    }
  }

  if (last_source_report_->lifecycle == NodeLifecycle::Fenced ||
      last_source_report_->lifecycle == NodeLifecycle::Retired) {
    if (handoff_->phase == HandoffPhase::ReadinessVerified) {
      const Status phased = record_phase(
          HandoffPhase::PredecessorFenced,
          "reconciled: the predecessor reports it is already fenced");
      if (!phased.ok()) {
        return phased;
      }
      explanation.add_note("predecessor reports a fenced lifecycle");
    }
  }

  if (!is_terminal_phase(handoff_->phase) && handoff_->phase < HandoffPhase::PredecessorFenced &&
      incarnation_provably_replaced(handoff_->predecessor, last_source_report_->incarnation)) {
    const Status restarted = restart_handoff(last_source_report_->incarnation, handoff_->successor,
                                             HandoffPhase::NotStarted,
                                             "reconciled after restart: the predecessor is a new "
                                             "incarnation");
    if (!restarted.ok()) {
      return restarted;
    }
    explanation.add_note("handoff restarted for the new predecessor incarnation");
  }

  EpochEvent event;
  event.kind = EpochEventKind::Reconciliation;
  event.shard = manifest_->shard;
  event.epoch = authority_.current_epoch(manifest_->shard).value_or(EpochNumber::invalid());
  event.generation = authority_.current_generation(manifest_->shard).value_or(Generation::invalid());
  event.at_ms = clock_->now_ms();
  event.payload = Json::object({{"handoff_phase", Json(std::string(::fabric::evolution::to_string(
                                                  handoff_->phase)))},
                                {"controller_boot", Json(controller_boot_.value())}});
  const Status logged = epoch_log_.append(event);
  if (!logged.ok()) {
    return logged;
  }
  const Status saved = save_state();
  if (!saved.ok()) {
    return saved;
  }
  explanation.set_outcome(DecisionOutcome::Accepted);
  explanation.set_selected_action("reconciled");
  explanation.set_resulting_state(std::string(::fabric::evolution::to_string(handoff_->phase)));
  return status_json_locked();
}

Json EvolutionController::status_json_locked() const {
  Json out = Json::object();
  out.set("controller", Json(options_.controller_id.str()));
  out.set("controller_incarnation", ::fabric::evolution::to_json(controller_incarnation_));
  out.set("controller_boot", Json(controller_boot_.value()));
  out.set("state_recovered_from_backup", Json(recovered_from_backup_));
  out.set("epoch_journal_records", Json(epoch_log_.size()));
  out.set("epoch_journal_damaged_tail", Json(epoch_log_.tail_damaged()));
  if (!epoch_log_.diagnostic().empty()) {
    out.set("epoch_journal_diagnostic", Json(epoch_log_.diagnostic()));
  }
  if (manifest_.has_value()) {
    out.set("manifest", ::fabric::evolution::to_json(*manifest_));
  }
  if (campaign_.has_value()) {
    out.set("campaign", ::fabric::evolution::to_json(*campaign_));
  }
  if (handoff_.has_value()) {
    out.set("handoff", ::fabric::evolution::to_json(*handoff_));
  }
  if (last_source_report_.has_value()) {
    out.set("source_report", last_source_report_->to_json());
  }
  if (last_target_report_.has_value()) {
    out.set("target_report", last_target_report_->to_json());
  }
  const WindowStatus window = window_status_locked();
  Json window_json = Json::object();
  window_json.set("open", Json(window.open));
  window_json.set("operations", Json(window.operations));
  window_json.set("operation_limit", Json(window.operation_limit));
  window_json.set("elapsed_ms", Json(window.elapsed_ms));
  window_json.set("duration_limit_ms", Json(window.duration_limit_ms));
  window_json.set("exhausted", Json(window.exhausted));
  window_json.set("reason", Json(window.reason));
  out.set("window", std::move(window_json));
  out.set("last_deferral_reason", Json(last_deferral_reason_));
  out.set("authority", authority_json_locked());
  return out;
}

Json EvolutionController::authority_json_locked() const {
  Json out = Json::array();
  for (const AuthorityView& view : authority_.all_views()) {
    Json entry = Json::object();
    entry.set("shard", Json(view.shard.str()));
    entry.set("epoch", Json(view.epoch.value()));
    entry.set("generation", Json(view.generation.value()));
    entry.set("mutating_authority_count", Json(static_cast<std::uint64_t>(view.mutating_authority_count())));
    entry.set("mutation_slot_free", Json(view.mutation_slot_free()));
    if (view.mutating.has_value()) {
      entry.set("mutating", ::fabric::evolution::to_json(*view.mutating));
    }
    if (view.slot_holder.has_value()) {
      entry.set("slot_holder", ::fabric::evolution::to_json(*view.slot_holder));
    }
    Json read_only = Json::array();
    for (const AuthorityLease& lease : view.read_only) {
      read_only.push_back(::fabric::evolution::to_json(lease));
    }
    entry.set("read_only", std::move(read_only));
    Json fences = Json::array();
    for (const FenceRecord& fence : view.fences) {
      fences.push_back(::fabric::evolution::to_json(fence));
    }
    entry.set("fences", std::move(fences));
    out.push_back(std::move(entry));
  }
  return out;
}

Json EvolutionController::epoch_json_locked() const {
  Json out = Json::object();
  Json events = Json::array();
  const auto& all = epoch_log_.events();
  const std::size_t limit = 512;
  const std::size_t start = all.size() > limit ? all.size() - limit : 0;
  for (std::size_t index = start; index < all.size(); ++index) {
    events.push_back(::fabric::evolution::to_json(all[index]));
  }
  out.set("events", std::move(events));
  out.set("records", Json(epoch_log_.size()));
  out.set("replayed_records", Json(epoch_log_.replayed_records()));
  out.set("damaged_tail", Json(epoch_log_.tail_damaged()));
  return out;
}

Json EvolutionController::handoff_json_locked() const {
  Json out = Json::object();
  if (handoff_.has_value()) {
    out.set("handoff", ::fabric::evolution::to_json(*handoff_));
    out.set("phase", Json(std::string(::fabric::evolution::to_string(handoff_->phase))));
    out.set("phase_rank", Json(phase_rank(handoff_->phase)));
  } else {
    out.set("phase", Json("none"));
  }
  if (campaign_.has_value()) {
    out.set("campaign_state", Json(std::string(::fabric::evolution::to_string(campaign_->state))));
  }
  return out;
}

Result<Json> EvolutionController::explain_topic_locked(const std::string& topic) const {
  if (topic == "handoff" || topic == "campaign") {
    return handoff_json_locked();
  }
  if (topic == "authority") {
    return authority_json_locked();
  }
  if (topic == "epoch") {
    return epoch_json_locked();
  }
  if (topic == "window" || topic == "mixed-version") {
    const WindowStatus window = window_status_locked();
    Json out = Json::object();
    out.set("open", Json(window.open));
    out.set("operations", Json(window.operations));
    out.set("operation_limit", Json(window.operation_limit));
    out.set("elapsed_ms", Json(window.elapsed_ms));
    out.set("duration_limit_ms", Json(window.duration_limit_ms));
    out.set("exhausted", Json(window.exhausted));
    out.set("reason", Json(window.reason));
    if (manifest_.has_value()) {
      const MixedVersionWindow effective = manifest_->effective_window();
      out.set("effective_max_operations", Json(effective.max_operations));
      out.set("effective_max_duration_ms", Json(effective.max_duration_ms));
      out.set("declared_max_operations", Json(manifest_->window.max_operations));
      out.set("declared_max_duration_ms", Json(manifest_->window.max_duration_ms));
    }
    return out;
  }
  if (topic == "compatibility") {
    if (!manifest_.has_value()) {
      return Status::error(ErrorCode::NotFound, "no manifest has been planned");
    }
    const EvidenceCheck evidence =
        verify_evidence(manifest_->compatibility, *registry_, options_.evidence_policy);
    Json out = Json::object();
    out.set("declared", ::fabric::evolution::to_json(manifest_->compatibility));
    out.set("satisfied", Json(evidence.satisfied));
    out.set("refreshed", Json(evidence.refreshed));
    out.set("rationale", Json(evidence.rationale));
    out.set("registry", Json(registry_->name()));
    Json differences = Json::array();
    for (const std::string& difference : evidence.differences) {
      differences.push_back(Json(difference));
    }
    out.set("differences", std::move(differences));
    return out;
  }
  return Status::error(ErrorCode::NotFound, "unknown explanation topic: " + topic);
}

Result<Json> EvolutionController::admin(const RpcRequest& request) {
  if (request.op == "status") {
    return status_json();
  }
  if (request.op == "authority") {
    return authority_json();
  }
  if (request.op == "epoch") {
    return epoch_json();
  }
  if (request.op == "handoff") {
    return handoff_json();
  }
  if (request.op == "explain") {
    const auto topic = json_string(request.body, "topic");
    if (!topic.has_value()) {
      return invalid_argument("explain requires a topic");
    }
    return explain_topic(*topic);
  }
  if (request.op == "plan") {
    const Json* manifest = request.body.find("manifest");
    if (manifest == nullptr) {
      return invalid_argument("plan requires a manifest document");
    }
    auto parsed = manifest_from_json(*manifest);
    if (!parsed.ok()) {
      return parsed.status();
    }
    Explanation explanation("campaign.plan", "fabric-evolution.manifest-admission.v1");
    auto result = plan(parsed.value(), explanation);
    if (!result.ok()) {
      Json body = Json::object();
      body.set("explanation", explanation.to_json());
      return Status::error(result.status().code(), result.status().message(), std::move(body));
    }
    Json body = result.take();
    body.set("explanation", explanation.to_json());
    return body;
  }

  const auto run = [&](const std::string& name, auto&& callable) -> Result<Json> {
    Explanation explanation(name, "fabric-evolution.operation.v1");
    auto result = callable(explanation);
    Json explanation_json = explanation.to_json();
    if (!result.ok()) {
      Json body = Json::object();
      body.set("explanation", std::move(explanation_json));
      return Status::error(result.status().code(), result.status().message(), std::move(body));
    }
    Json body = result.take();
    body.set("explanation", std::move(explanation_json));
    return body;
  };

  if (request.op == "preflight") {
    return run("campaign.preflight", [this](Explanation& e) { return preflight(e); });
  }
  if (request.op == "start") {
    return run("campaign.start", [this](Explanation& e) { return start(e); });
  }
  if (request.op == "advance") {
    return run("handoff.advance", [this](Explanation& e) { return advance(e); });
  }
  if (request.op == "run") {
    return run("handoff.run", [this](Explanation& e) { return run_to_completion(e); });
  }
  if (request.op == "pause") {
    return run("campaign.pause", [this](Explanation& e) { return pause(e); });
  }
  if (request.op == "resume") {
    return run("campaign.resume", [this](Explanation& e) { return resume(e); });
  }
  if (request.op == "abort") {
    return run("campaign.abort", [this](Explanation& e) { return abort(e); });
  }
  if (request.op == "reconcile") {
    return run("controller.reconcile", [this](Explanation& e) { return reconcile(e); });
  }
  return Status::error(ErrorCode::NotFound, "unknown admin operation: " + request.op);
}


// ---------------------------------------------------------------------------
// Public entry points. Each acquires the controller lock exactly once and
// delegates to the corresponding _locked implementation, so a public call can
// never re-enter the mutex.
// ---------------------------------------------------------------------------
Result<Json> EvolutionController::plan(const EvolutionManifest& manifest, Explanation& explanation) {
  std::lock_guard<std::mutex> lock(mutex_);
  return plan_locked(manifest, explanation);
}

Result<Json> EvolutionController::preflight(Explanation& explanation) {
  std::lock_guard<std::mutex> lock(mutex_);
  return preflight_locked(explanation);
}

Result<Json> EvolutionController::start(Explanation& explanation) {
  std::lock_guard<std::mutex> lock(mutex_);
  return start_locked(explanation);
}

Result<Json> EvolutionController::advance(Explanation& explanation) {
  std::lock_guard<std::mutex> lock(mutex_);
  return advance_locked(explanation);
}

Result<Json> EvolutionController::run_to_completion(Explanation& explanation) {
  std::lock_guard<std::mutex> lock(mutex_);
  return run_to_completion_locked(explanation);
}

Result<Json> EvolutionController::pause(Explanation& explanation) {
  std::lock_guard<std::mutex> lock(mutex_);
  return pause_locked(explanation);
}

Result<Json> EvolutionController::resume(Explanation& explanation) {
  std::lock_guard<std::mutex> lock(mutex_);
  return resume_locked(explanation);
}

Result<Json> EvolutionController::abort(Explanation& explanation) {
  std::lock_guard<std::mutex> lock(mutex_);
  return abort_locked(explanation);
}

Result<Json> EvolutionController::reconcile(Explanation& explanation) {
  std::lock_guard<std::mutex> lock(mutex_);
  return reconcile_locked(explanation);
}

Json EvolutionController::status_json() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return status_json_locked();
}

Json EvolutionController::authority_json() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return authority_json_locked();
}

Json EvolutionController::epoch_json() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return epoch_json_locked();
}

Json EvolutionController::handoff_json() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return handoff_json_locked();
}

Result<Json> EvolutionController::explain_topic(const std::string& topic) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return explain_topic_locked(topic);
}

Result<EvolutionManifest> load_manifest_file(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return Status::error(ErrorCode::IoFailure, "cannot open manifest file",
                         Json::object({{"path", Json(path)}}));
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return manifest_from_json_text(buffer.str());
}

}  // namespace fabric::evolution
