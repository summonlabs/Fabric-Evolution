// Fabric Evolution — authority leases, fencing and the per-shard authority registry.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The registry is the single decision point for "who may mutate shard S right
// now". It maintains three invariants:
//
//   I1  At most one lease per shard is in mode Mutating and state Active.
//   I2  Generation is strictly monotonic; every authority change bumps it, so a
//       claim minted under an older generation can never be replayed into the
//       current one.
//   I3  A mutating lease is only granted to a new holder once the previous
//       holder's lease is Fenced (acknowledged), Expired or Revoked. A
//       predecessor that has merely been asked to fence keeps the slot busy,
//       which is what makes "fence predecessor" a real barrier rather than a
//       best-effort notification.
//
// Multiple holders may simultaneously hold ReadOnlyShared leases. That is the
// only phase in which more than one component is authoritative, and it carries
// no mutation authority by construction.

#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "fabric/evolution/clock.hpp"
#include "fabric/evolution/ids.hpp"
#include "fabric/evolution/status.hpp"

namespace fabric::evolution {

enum class AuthorityMode : std::uint8_t {
  None = 0,            // no authority: the component must refuse every operation
  ReadOnlyShared = 1,  // may serve reads; may not mutate
  Mutating = 2,        // may mutate replicated state for the shard
};

[[nodiscard]] std::string_view to_string(AuthorityMode mode) noexcept;
[[nodiscard]] bool authority_allows_mutation(AuthorityMode mode) noexcept;
[[nodiscard]] bool authority_allows_read(AuthorityMode mode) noexcept;

enum class LeaseState : std::uint8_t {
  Active = 0,   // live and usable (subject to attestation)
  Fencing = 1,  // fence issued, holder has not yet acknowledged: slot stays busy
  Fenced = 2,   // fence acknowledged: holder may no longer act
  Expired = 3,  // lease TTL elapsed without renewal
  Revoked = 4,  // withdrawn by an epoch change or an explicit revocation
};

[[nodiscard]] std::string_view to_string(LeaseState state) noexcept;

struct AuthorityLease {
  LeaseId id;
  ShardId shard;
  IncarnationId holder;
  EpochNumber epoch;
  Generation generation;
  AuthorityToken token;
  AuthorityMode mode = AuthorityMode::None;
  LeaseState state = LeaseState::Active;
  TimestampMs issued_at_ms = 0;
  TimestampMs expires_at_ms = 0;
  // False for leases restored from durable storage: such a lease is *not* usable
  // until its holder re-attests that it still holds the same token. Persisted
  // evidence never becomes current merely because it could be deserialized.
  bool attested = false;
  std::uint64_t attestation_seq = 0;
  CampaignId campaign;  // campaign that issued the lease, if any
  HandoffId handoff;    // handoff that issued the lease, if any
  std::string reason;   // deterministic record of why the lease exists

  [[nodiscard]] bool is_live(TimestampMs now_ms) const noexcept;
  [[nodiscard]] bool authorizes_mutation(TimestampMs now_ms) const noexcept;
  [[nodiscard]] bool authorizes_read(TimestampMs now_ms) const noexcept;
};

struct FenceRecord {
  FenceId id;
  ShardId shard;
  IncarnationId target;
  EpochNumber epoch;
  Generation generation;
  LeaseId fenced_lease;
  TimestampMs issued_at_ms = 0;
  TimestampMs acknowledged_at_ms = 0;
  bool acknowledged = false;
  std::string reason;
};

struct GrantRequest {
  ShardId shard;
  IncarnationId holder;
  AuthorityMode mode = AuthorityMode::Mutating;
  std::uint64_t ttl_ms = 5000;
  CampaignId campaign;
  HandoffId handoff;
  std::string reason;
};

struct AuthorityView {
  ShardId shard;
  EpochNumber epoch;
  Generation generation;
  // The single lease that currently permits mutation, if any.
  std::optional<AuthorityLease> mutating;
  // The lease occupying the mutation slot. It is present while a fence is
  // outstanding, because the registry refuses to hand the slot to a successor
  // until the predecessor has acknowledged the fence or its lease has expired.
  std::optional<AuthorityLease> slot_holder;
  std::vector<AuthorityLease> read_only;
  std::vector<FenceRecord> fences;

  // Number of leases that currently permit mutation. Always 0 or 1; exposed so
  // that proof obligations can assert it directly.
  [[nodiscard]] std::size_t mutating_authority_count() const noexcept {
    return mutating.has_value() ? 1U : 0U;
  }
  [[nodiscard]] bool mutation_slot_free() const noexcept { return !slot_holder.has_value(); }
};

class AuthorityRegistry {
 public:
  explicit AuthorityRegistry(Clock& clock) : clock_(&clock) {}

  [[nodiscard]] bool has_shard(const ShardId& shard) const;
  [[nodiscard]] std::vector<ShardId> shards() const;
  [[nodiscard]] std::optional<EpochNumber> current_epoch(const ShardId& shard) const;
  [[nodiscard]] std::optional<Generation> current_generation(const ShardId& shard) const;

  // Creates the initial authority record for a shard. Fails if it already exists.
  Status establish(const ShardId& shard, EpochNumber epoch);

  // Highest valid lease tokens are minted from this factory so that the
  // registry fully controls the authority namespace.
  void set_token_source(class TokenSource* source) { tokens_ = source; }

  [[nodiscard]] Result<AuthorityLease> grant(const GrantRequest& request);
  [[nodiscard]] Status renew(const ShardId& shard, const LeaseId& lease, std::uint64_t ttl_ms);
  [[nodiscard]] Status request_fence(const ShardId& shard, const IncarnationId& target,
                                     std::string reason);
  [[nodiscard]] Status acknowledge_fence(const ShardId& shard, const IncarnationId& holder,
                                         const FenceId& fence);
  [[nodiscard]] Status revoke(const ShardId& shard, const LeaseId& lease, std::string reason);
  [[nodiscard]] Status expire_due();
  // A restored lease becomes usable again only when its holder proves possession
  // of the exact token the registry recorded.
  [[nodiscard]] Status reattest(const ShardId& shard, const IncarnationId& holder,
                                const AuthorityToken& token);
  [[nodiscard]] Status advance_epoch(const ShardId& shard, EpochNumber epoch);

  [[nodiscard]] Status validate_claim(const AuthorityClaim& claim, AuthorityMode required) const;
  [[nodiscard]] Result<AuthorityView> view(const ShardId& shard) const;
  [[nodiscard]] std::vector<AuthorityView> all_views() const;

  [[nodiscard]] Json to_json() const;
  Status load(const Json& value);
  void clear();

 private:
  struct ShardAuthority {
    ShardId shard;
    EpochNumber epoch;
    Generation generation;
    std::vector<AuthorityLease> leases;
    std::vector<FenceRecord> fences;
    LeaseId next_lease_id = LeaseId::from_value(1);
    FenceId next_fence_id = FenceId::from_value(1);
  };

  [[nodiscard]] ShardAuthority* find(const ShardId& shard);
  [[nodiscard]] const ShardAuthority* find(const ShardId& shard) const;
  [[nodiscard]] std::optional<AuthorityLease> active_mutating(const ShardAuthority& authority) const;
  [[nodiscard]] std::optional<FenceRecord> pending_fence(const ShardAuthority& authority) const;
  void expire_in_place(ShardAuthority& authority, TimestampMs now_ms);
  [[nodiscard]] AuthorityToken mint_token();

  Clock* clock_;
  TokenSource* tokens_ = nullptr;
  std::map<ShardId, ShardAuthority> shards_;
  LeaseId next_lease_id_ = LeaseId::from_value(1);
  FenceId next_fence_id_ = FenceId::from_value(1);
};

// Deterministic token minting. Production uses a random source; tests use a
// seeded source so that authority transitions are reproducible.
class TokenSource {
 public:
  TokenSource() = default;
  virtual ~TokenSource();
  TokenSource(const TokenSource&) = delete;
  TokenSource& operator=(const TokenSource&) = delete;
  [[nodiscard]] virtual AuthorityToken next_token() = 0;
};

class RandomTokenSource final : public TokenSource {
 public:
  RandomTokenSource();
  [[nodiscard]] AuthorityToken next_token() override;

 private:
  std::uint64_t state_[4] = {};
};

class SeededTokenSource final : public TokenSource {
 public:
  explicit SeededTokenSource(std::uint64_t seed = 1) : state_(seed == 0 ? 1 : seed) {}
  [[nodiscard]] AuthorityToken next_token() override;

 private:
  std::uint64_t state_;
};

[[nodiscard]] Json to_json(const AuthorityLease& lease);
[[nodiscard]] Result<AuthorityLease> authority_lease_from_json(const Json& value);
[[nodiscard]] Json to_json(const FenceRecord& fence);
[[nodiscard]] Result<FenceRecord> fence_record_from_json(const Json& value);

}  // namespace fabric::evolution
