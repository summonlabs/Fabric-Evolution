// Fabric Evolution — protocol feature identities and negotiated feature sets.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// A feature is a named capability that only becomes usable when every required
// participant for the campaign has declared it. Declaring a bit does not imply
// that consensus, quorum or any other guarantee exists: every feature listed
// here is implemented by this repository and exercised by its test suite.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fabric::evolution {

enum class Feature : std::uint8_t {
  // Predecessor state can be shipped to a successor as an integrity-checked snapshot.
  SnapshotTransfer = 0,
  // A successor can replay the incremental log from a snapshot boundary.
  IncrementalCatchUp = 1,
  // A predecessor may keep read-only authority while the successor takes mutating authority.
  ReadOnlySharedAuthority = 2,
  // Versioned state migration steps are transported and applied by the node.
  SchemaMigration = 3,
  // Explicit protocol negotiation handshake before any authority changes.
  ProtocolNegotiation = 4,
  // Campaign may proceed past a failed rollback by going forward instead.
  ForwardRecovery = 5,
  // Persisted records carry a checksum plus a content digest.
  ChecksummedRecords = 6,
  // Leases loaded from disk must be re-attested by their holder before use.
  LeaseReattestation = 7,
  // Stale epoch/generation/incarnation claims are rejected rather than ignored.
  FencedStaleRejection = 8,
  // A schema migration step may declare a reverse function.
  ReverseMigration = 9,
  // Snapshot encoding is compact (structural sharing of repeated keys).
  CompactSnapshotEncoding = 10,
  // Catch-up frames may be pipelined by the controller.
  PipelinedCatchUp = 11,
  // Components attest their epoch/generation on every reply.
  EpochAttestation = 12,
  // Handoff phase transitions are journaled durably before being acknowledged.
  DurableHandoffJournal = 13,
};

inline constexpr std::uint8_t kFeatureCount = 14;

[[nodiscard]] std::string_view feature_name(Feature feature) noexcept;
[[nodiscard]] std::optional<Feature> feature_from_name(std::string_view name) noexcept;

// Bounded set of features. Unknown bits are preserved on decode so that a peer
// running a newer build never has its capability silently misread as absent.
class FeatureSet {
 public:
  constexpr FeatureSet() noexcept = default;
  explicit constexpr FeatureSet(std::uint64_t bits) noexcept : bits_(bits) {}

  [[nodiscard]] static constexpr FeatureSet none() noexcept { return FeatureSet(); }
  [[nodiscard]] static constexpr FeatureSet of(Feature feature) noexcept {
    return FeatureSet(1ULL << static_cast<std::uint64_t>(feature));
  }

  [[nodiscard]] constexpr std::uint64_t bits() const noexcept { return bits_; }
  [[nodiscard]] constexpr bool empty() const noexcept { return bits_ == 0; }
  [[nodiscard]] constexpr bool contains(Feature feature) const noexcept {
    return (bits_ & (1ULL << static_cast<std::uint64_t>(feature))) != 0;
  }
  [[nodiscard]] constexpr bool contains_all(const FeatureSet& other) const noexcept {
    return (bits_ & other.bits_) == other.bits_;
  }
  [[nodiscard]] constexpr bool intersects(const FeatureSet& other) const noexcept {
    return (bits_ & other.bits_) != 0;
  }

  [[nodiscard]] constexpr FeatureSet intersect(const FeatureSet& other) const noexcept {
    return FeatureSet(bits_ & other.bits_);
  }
  [[nodiscard]] constexpr FeatureSet unite(const FeatureSet& other) const noexcept {
    return FeatureSet(bits_ | other.bits_);
  }
  [[nodiscard]] constexpr FeatureSet without(const FeatureSet& other) const noexcept {
    return FeatureSet(bits_ & ~other.bits_);
  }

  constexpr FeatureSet& add(Feature feature) noexcept {
    bits_ |= 1ULL << static_cast<std::uint64_t>(feature);
    return *this;
  }

  [[nodiscard]] std::vector<std::string> names() const;
  [[nodiscard]] static std::optional<FeatureSet> from_names(const std::vector<std::string>& names,
                                                            std::string& error);

  friend constexpr bool operator==(FeatureSet, FeatureSet) noexcept = default;

 private:
  std::uint64_t bits_ = 0;
};

}  // namespace fabric::evolution
