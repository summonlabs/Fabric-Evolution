// Fabric Evolution — protocol feature identities (implementation).
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric/evolution/features.hpp"

#include <array>
#include <utility>

namespace fabric::evolution {
namespace {

using Entry = std::pair<Feature, std::string_view>;

constexpr std::array<Entry, kFeatureCount> kFeatureTable = {{
    {Feature::SnapshotTransfer, "snapshot_transfer"},
    {Feature::IncrementalCatchUp, "incremental_catch_up"},
    {Feature::ReadOnlySharedAuthority, "read_only_shared_authority"},
    {Feature::SchemaMigration, "schema_migration"},
    {Feature::ProtocolNegotiation, "protocol_negotiation"},
    {Feature::ForwardRecovery, "forward_recovery"},
    {Feature::ChecksummedRecords, "checksummed_records"},
    {Feature::LeaseReattestation, "lease_reattestation"},
    {Feature::FencedStaleRejection, "fenced_stale_rejection"},
    {Feature::ReverseMigration, "reverse_migration"},
    {Feature::CompactSnapshotEncoding, "compact_snapshot_encoding"},
    {Feature::PipelinedCatchUp, "pipelined_catch_up"},
    {Feature::EpochAttestation, "epoch_attestation"},
    {Feature::DurableHandoffJournal, "durable_handoff_journal"},
}};

}  // namespace

std::string_view feature_name(Feature feature) noexcept {
  for (const Entry& entry : kFeatureTable) {
    if (entry.first == feature) {
      return entry.second;
    }
  }
  return "unknown";
}

std::optional<Feature> feature_from_name(std::string_view name) noexcept {
  for (const Entry& entry : kFeatureTable) {
    if (entry.second == name) {
      return entry.first;
    }
  }
  return std::nullopt;
}

std::vector<std::string> FeatureSet::names() const {
  std::vector<std::string> out;
  for (const Entry& entry : kFeatureTable) {
    if (contains(entry.first)) {
      out.emplace_back(entry.second);
    }
  }
  return out;
}

std::optional<FeatureSet> FeatureSet::from_names(const std::vector<std::string>& names,
                                                 std::string& error) {
  FeatureSet set;
  for (const std::string& name : names) {
    const auto feature = feature_from_name(name);
    if (!feature.has_value()) {
      error = "unknown feature: " + name;
      return std::nullopt;
    }
    set.add(*feature);
  }
  return set;
}

}  // namespace fabric::evolution
