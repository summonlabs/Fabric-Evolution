// Fabric Evolution — domain identities for the control-plane evolution runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every identity below is a distinct C++ type. Component, shard, campaign,
// epoch, generation, incarnation, lease and authority token values cannot be
// interchanged by accident, and 0 / nil is reserved as "unset".

#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "fabric/evolution/json.hpp"
#include "fabric/evolution/status.hpp"
#include "fabric/evolution/strong_types.hpp"
#include "fabric/evolution/versions.hpp"

namespace fabric::evolution {

// Tag declarations — never instantiated, they only distinguish types.
struct ComponentTag;
struct ShardTag;
struct DomainTag;
struct CampaignTag;
struct ManifestTag;
struct MigrationStepTag;
struct HandoffTag;
struct LeaseTag;
struct EpochTag;
struct GenerationTag;
struct BootTag;
struct LsnTag;
struct SnapshotTag;
struct AttemptTag;
struct RevisionTag;
struct AuthorityTokenTag;
struct IncarnationTag;
struct RequestTag;
struct OperationTag;
struct FenceTag;
struct JournalTag;

// Textual identities. Component names are used as durable keys, so they are
// validated once at the boundary and are immutable afterwards.
using ComponentId = TaggedName<ComponentTag>;
using ShardId = TaggedName<ShardTag>;
using DomainId = TaggedName<DomainTag>;
using CampaignId = TaggedName<CampaignTag>;
using ManifestId = TaggedName<ManifestTag>;
using MigrationStepId = TaggedName<MigrationStepTag>;

// Monotonic numeric identities.
using EpochNumber = Tagged<EpochTag>;            // one authority epoch: a fenced, durable era of a shard
using Generation = Tagged<GenerationTag>;        // increments on every authority change within/between epochs
using BootCounter = Tagged<BootTag>;             // per-component process start counter, durable
using LogSequenceNumber = Tagged<LsnTag>;        // replicated state position
using HandoffId = Tagged<HandoffTag>;            // identifies one handoff attempt sequence
using LeaseId = Tagged<LeaseTag>;                // identifies one authority lease
using SnapshotId = Tagged<SnapshotTag>;          // identifies one snapshot image
using FenceId = Tagged<FenceTag>;                // identifies one fencing action
using AttemptNumber = Tagged<AttemptTag, std::uint32_t>;
using Revision = Tagged<RevisionTag>;            // record revision for optimistic concurrency
using OperationId = Tagged<OperationTag>;        // client-visible replicated operation counter

// 128-bit opaque identities. These are unguessable and never reused.
using AuthorityToken = TaggedUuid<AuthorityTokenTag>;
using IncarnationUuid = TaggedUuid<IncarnationTag>;
using RequestId = TaggedUuid<RequestTag>;

[[nodiscard]] inline std::string to_string(const ComponentId& id) { return id.str(); }
[[nodiscard]] inline std::string to_string(const ShardId& id) { return id.str(); }

// ---------------------------------------------------------------------------
// IncarnationId — the boot identity of one component process. A restarted
// process is a *different* incarnation: same ComponentId, new IncarnationUuid,
// strictly greater BootCounter. Authority and every mutating message are bound
// to an incarnation, which is what makes fresh-incarnation fencing possible.
// ---------------------------------------------------------------------------
class IncarnationId {
 public:
  IncarnationId() = default;
  IncarnationId(ComponentId component, IncarnationUuid uuid, BootCounter boot)
      : component_(std::move(component)), uuid_(uuid), boot_(boot) {}

  [[nodiscard]] const ComponentId& component() const noexcept { return component_; }
  [[nodiscard]] const IncarnationUuid& uuid() const noexcept { return uuid_; }
  [[nodiscard]] BootCounter boot() const noexcept { return boot_; }
  [[nodiscard]] bool is_valid() const noexcept {
    return component_.is_valid() && uuid_.is_valid() && boot_.is_valid();
  }

  [[nodiscard]] std::string to_string() const;
  [[nodiscard]] static std::optional<IncarnationId> parse(std::string_view text);

  friend bool operator==(const IncarnationId&, const IncarnationId&) noexcept = default;
  friend std::strong_ordering operator<=>(const IncarnationId& lhs, const IncarnationId& rhs) noexcept {
    if (const auto cmp = lhs.component_ <=> rhs.component_; cmp != 0) {
      return cmp;
    }
    if (const auto cmp = lhs.boot_ <=> rhs.boot_; cmp != 0) {
      return cmp;
    }
    return lhs.uuid_.value() <=> rhs.uuid_.value();
  }

 private:
  ComponentId component_;
  IncarnationUuid uuid_;
  BootCounter boot_;
};

// ---------------------------------------------------------------------------
// AuthorityClaim — the evidence a component presents when it acts for a shard.
// Every mutating request carries one; the controller and the nodes reject a
// claim that is not exactly the current authority.
// ---------------------------------------------------------------------------
struct AuthorityClaim {
  ShardId shard;
  IncarnationId incarnation;
  EpochNumber epoch;
  Generation generation;
  AuthorityToken token;

  [[nodiscard]] bool is_valid() const noexcept {
    return shard.is_valid() && incarnation.is_valid() && epoch.is_valid() && generation.is_valid() &&
           token.is_valid();
  }
  friend bool operator==(const AuthorityClaim&, const AuthorityClaim&) noexcept = default;
};

// JSON encodings. Decoders are strict: a missing or malformed identity is an
// error, never a default value.
[[nodiscard]] Json to_json(const IncarnationId& incarnation);
[[nodiscard]] Result<IncarnationId> incarnation_from_json(const Json& value);
[[nodiscard]] Json to_json(const AuthorityClaim& claim);
[[nodiscard]] Result<AuthorityClaim> authority_claim_from_json(const Json& value);

[[nodiscard]] Json to_json(const ComponentId& id);
[[nodiscard]] Json to_json(const ShardId& id);
[[nodiscard]] Result<ComponentId> component_id_from_json(const Json& value);
[[nodiscard]] Result<ShardId> shard_id_from_json(const Json& value);

}  // namespace fabric::evolution

namespace std {

template <class Tag, class Policy>
struct hash<fabric::evolution::TaggedName<Tag, Policy>> {
  [[nodiscard]] size_t operator()(const fabric::evolution::TaggedName<Tag, Policy>& value) const noexcept {
    return std::hash<std::string_view>{}(value.view());
  }
};

template <class Tag, class Rep>
struct hash<fabric::evolution::Tagged<Tag, Rep>> {
  [[nodiscard]] size_t operator()(const fabric::evolution::Tagged<Tag, Rep>& value) const noexcept {
    return std::hash<Rep>{}(value.value());
  }
};

template <class Tag>
struct hash<fabric::evolution::TaggedUuid<Tag>> {
  [[nodiscard]] size_t operator()(const fabric::evolution::TaggedUuid<Tag>& value) const noexcept {
    const auto& bytes = value.value().bytes();
    size_t seed = 1469598103934665603ULL;
    for (std::uint8_t byte : bytes) {
      seed ^= static_cast<size_t>(byte);
      seed *= 1099511628211ULL;
    }
    return seed;
  }
};

}  // namespace std
