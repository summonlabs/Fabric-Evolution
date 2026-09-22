// Fabric Evolution — durable epoch journal (implementation).
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric/evolution/epoch_log.hpp"

#include "fabric/evolution/persistence.hpp"

namespace fabric::evolution {

std::string_view to_string(EpochEventKind kind) noexcept {
  switch (kind) {
    case EpochEventKind::ShardEstablished:
      return "shard_established";
    case EpochEventKind::LeaseGranted:
      return "lease_granted";
    case EpochEventKind::LeaseRenewed:
      return "lease_renewed";
    case EpochEventKind::FenceRequested:
      return "fence_requested";
    case EpochEventKind::FenceAcknowledged:
      return "fence_acknowledged";
    case EpochEventKind::LeaseRevoked:
      return "lease_revoked";
    case EpochEventKind::EpochAdvanced:
      return "epoch_advanced";
    case EpochEventKind::HandoffPhase:
      return "handoff_phase";
    case EpochEventKind::CampaignTransition:
      return "campaign_transition";
    case EpochEventKind::MigrationApplied:
      return "migration_applied";
    case EpochEventKind::Reconciliation:
      return "reconciliation";
    case EpochEventKind::Refusal:
      return "refusal";
  }
  return "unknown";
}

std::optional<EpochEventKind> epoch_event_kind_from(std::string_view text) {
  for (std::uint8_t index = 0; index <= static_cast<std::uint8_t>(EpochEventKind::Refusal); ++index) {
    const auto kind = static_cast<EpochEventKind>(index);
    if (to_string(kind) == text) {
      return kind;
    }
  }
  return std::nullopt;
}

Json to_json(const EpochEvent& event) {
  Json out = Json::object();
  out.set("kind", Json(std::string(::fabric::evolution::to_string(event.kind))));
  out.set("shard", Json(event.shard.str()));
  out.set("epoch", Json(event.epoch.value()));
  out.set("generation", Json(event.generation.value()));
  out.set("at_ms", Json(event.at_ms));
  out.set("payload", event.payload);
  return out;
}

Result<EpochEvent> epoch_event_from_json(const Json& value) {
  if (!value.is_object()) {
    return malformed("epoch event must be an object");
  }
  EpochEvent event;
  const auto kind = json_string(value, "kind");
  if (!kind.has_value()) {
    return malformed("epoch event is missing kind");
  }
  auto parsed_kind = epoch_event_kind_from(*kind);
  if (!parsed_kind.has_value()) {
    return malformed("epoch event kind is invalid: " + *kind);
  }
  event.kind = *parsed_kind;
  const auto shard = json_string(value, "shard");
  if (shard.has_value()) {
    auto parsed_shard = ShardId::parse(*shard);
    if (!parsed_shard.has_value()) {
      return malformed("epoch event shard is invalid: " + *shard);
    }
    event.shard = *parsed_shard;
  }
  event.epoch = EpochNumber::from_value(json_u64_or(value, "epoch", 0));
  event.generation = Generation::from_value(json_u64_or(value, "generation", 0));
  event.at_ms = json_u64_or(value, "at_ms", 0);
  if (const Json* payload = value.find("payload")) {
    if (!payload->is_object()) {
      return malformed("epoch event payload must be an object");
    }
    event.payload = *payload;
  }
  return event;
}

std::string encode_epoch_event(const EpochEvent& event) { return to_json(event).dump(); }

EpochLog::EpochLog(std::string path, std::size_t capacity)
    : path_(std::move(path)), capacity_(capacity == 0 ? 1 : capacity) {}

Status EpochLog::append(const EpochEvent& event) {
  const std::string encoded = encode_epoch_event(event);
  const Status appended = append_record(path_, encoded);
  if (!appended.ok()) {
    return appended;
  }
  events_.push_back(event);
  if (events_.size() > capacity_ * 2) {
    return compact();
  }
  return Status::success();
}

Status EpochLog::replay() {
  events_.clear();
  replayed_records_ = 0;
  tail_damaged_ = false;
  diagnostic_.clear();
  auto report = read_records(path_);
  if (!report.ok()) {
    return report.status();
  }
  const LogReadReport& read = report.value();
  replayed_records_ = read.total_records;
  tail_damaged_ = read.truncated || read.damaged;
  diagnostic_ = read.diagnostic;
  for (const std::string& record : read.records) {
    const JsonParseResult parsed = parse_json(record);
    if (!parsed.ok()) {
      tail_damaged_ = true;
      diagnostic_ = "epoch journal record is not valid JSON: " + parsed.error;
      break;
    }
    auto event = epoch_event_from_json(*parsed.value);
    if (!event.ok()) {
      tail_damaged_ = true;
      diagnostic_ = event.status().message();
      break;
    }
    events_.push_back(event.take());
  }
  while (events_.size() > capacity_) {
    events_.erase(events_.begin());
  }
  return Status::success();
}

Status EpochLog::compact() {
  std::vector<EpochEvent> retained;
  const std::size_t keep = capacity_ / 2 == 0 ? 1 : capacity_ / 2;
  const std::size_t start = events_.size() > keep ? events_.size() - keep : 0;
  retained.reserve(events_.size() - start);
  std::string payload;
  for (std::size_t index = start; index < events_.size(); ++index) {
    retained.push_back(events_[index]);
    const std::string encoded = encode_epoch_event(events_[index]);
    if (encoded.size() > kMaxRecordBytes) {
      return Status::error(ErrorCode::BoundsExceeded, "epoch journal record exceeds the permitted size");
    }
    std::string header;
    header.append("FXR1", 4);
    append_u32_le(header, 1U);
    append_u64_le(header, static_cast<std::uint64_t>(encoded.size()));
    append_u32_le(header, crc32(encoded));
    append_u32_le(header, 0U);
    payload += header;
    payload += encoded;
  }
  const Status written = write_bytes_atomic(path_, payload);
  if (!written.ok()) {
    return written;
  }
  events_ = std::move(retained);
  return Status::success();
}

}  // namespace fabric::evolution
