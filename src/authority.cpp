// Fabric Evolution — authority leases, fencing and the per-shard registry.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric/evolution/authority.hpp"

#include <algorithm>
#include <random>

namespace fabric::evolution {
namespace {

constexpr std::uint64_t kMaxLeaseTtlMs = 24ULL * 60ULL * 60ULL * 1000ULL;
constexpr std::size_t kMaxLeaseHistoryPerShard = 256;
constexpr std::size_t kMaxFenceHistoryPerShard = 256;
constexpr std::size_t kMaxReadOnlyLeasesPerShard = 8;

[[nodiscard]] std::string_view mode_name(AuthorityMode mode) noexcept {
  switch (mode) {
    case AuthorityMode::None:
      return "none";
    case AuthorityMode::ReadOnlyShared:
      return "read_only_shared";
    case AuthorityMode::Mutating:
      return "mutating";
  }
  return "unknown";
}

[[nodiscard]] std::optional<AuthorityMode> mode_from_name(std::string_view name) {
  if (name == "none") {
    return AuthorityMode::None;
  }
  if (name == "read_only_shared") {
    return AuthorityMode::ReadOnlyShared;
  }
  if (name == "mutating") {
    return AuthorityMode::Mutating;
  }
  return std::nullopt;
}

[[nodiscard]] std::string_view lease_state_name(LeaseState state) noexcept {
  switch (state) {
    case LeaseState::Active:
      return "active";
    case LeaseState::Fencing:
      return "fencing";
    case LeaseState::Fenced:
      return "fenced";
    case LeaseState::Expired:
      return "expired";
    case LeaseState::Revoked:
      return "revoked";
  }
  return "unknown";
}

[[nodiscard]] std::optional<LeaseState> lease_state_from_name(std::string_view name) {
  if (name == "active") {
    return LeaseState::Active;
  }
  if (name == "fencing") {
    return LeaseState::Fencing;
  }
  if (name == "fenced") {
    return LeaseState::Fenced;
  }
  if (name == "expired") {
    return LeaseState::Expired;
  }
  if (name == "revoked") {
    return LeaseState::Revoked;
  }
  return std::nullopt;
}

// A lease that has passed its expiry is treated as expired for every decision,
// whether or not the transition has been flushed to durable storage yet.
[[nodiscard]] LeaseState effective_state(const AuthorityLease& lease, TimestampMs now_ms) noexcept {
  if ((lease.state == LeaseState::Active || lease.state == LeaseState::Fencing) &&
      now_ms >= lease.expires_at_ms) {
    return LeaseState::Expired;
  }
  return lease.state;
}

[[nodiscard]] bool blocks_mutation_slot(const AuthorityLease& lease, TimestampMs now_ms) noexcept {
  return lease.mode == AuthorityMode::Mutating && effective_state(lease, now_ms) != LeaseState::Fenced &&
         effective_state(lease, now_ms) != LeaseState::Expired &&
         effective_state(lease, now_ms) != LeaseState::Revoked;
}

}  // namespace

std::string_view to_string(AuthorityMode mode) noexcept { return mode_name(mode); }

bool authority_allows_mutation(AuthorityMode mode) noexcept { return mode == AuthorityMode::Mutating; }

bool authority_allows_read(AuthorityMode mode) noexcept {
  return mode == AuthorityMode::Mutating || mode == AuthorityMode::ReadOnlyShared;
}

std::string_view to_string(LeaseState state) noexcept { return lease_state_name(state); }

bool AuthorityLease::is_live(TimestampMs now_ms) const noexcept {
  return state == LeaseState::Active && now_ms < expires_at_ms;
}

bool AuthorityLease::authorizes_mutation(TimestampMs now_ms) const noexcept {
  return is_live(now_ms) && attested && mode == AuthorityMode::Mutating;
}

bool AuthorityLease::authorizes_read(TimestampMs now_ms) const noexcept {
  return is_live(now_ms) && attested && authority_allows_read(mode);
}

TokenSource::~TokenSource() = default;

RandomTokenSource::RandomTokenSource() {
  std::random_device device;
  for (std::uint64_t& word : state_) {
    word = (static_cast<std::uint64_t>(device()) << 32U) ^ static_cast<std::uint64_t>(device());
  }
  bool all_zero = true;
  for (std::uint64_t word : state_) {
    all_zero = all_zero && word == 0;
  }
  if (all_zero) {
    state_[0] = 0x9E3779B97F4A7C15ULL;
  }
}

AuthorityToken RandomTokenSource::next_token() {
  // xoshiro256** — small, fast, and never repeats within a process lifetime.
  const auto rotl = [](std::uint64_t value, int shift) {
    return (value << shift) | (value >> (64 - shift));
  };
  const std::uint64_t result = rotl(state_[1] * 5ULL, 7) * 9ULL;
  const std::uint64_t temp = state_[1] << 17U;
  state_[2] ^= state_[0];
  state_[3] ^= state_[1];
  state_[1] ^= state_[2];
  state_[0] ^= state_[3];
  state_[2] ^= temp;
  state_[3] = rotl(state_[3], 45);

  Uuid128::bytes_type bytes{};
  const std::uint64_t high = result;
  const std::uint64_t low = rotl(state_[0] + state_[3], 23);
  for (int index = 0; index < 8; ++index) {
    bytes[static_cast<std::size_t>(index)] =
        static_cast<std::uint8_t>((high >> (8U * static_cast<unsigned>(index))) & 0xFFU);
    bytes[static_cast<std::size_t>(index) + 8] =
        static_cast<std::uint8_t>((low >> (8U * static_cast<unsigned>(index))) & 0xFFU);
  }
  return AuthorityToken::from_value(Uuid128(bytes));
}

AuthorityToken SeededTokenSource::next_token() {
  // splitmix64 from a caller-supplied seed: reproducible authority transitions.
  state_ += 0x9E3779B97F4A7C15ULL;
  std::uint64_t z = state_;
  z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
  const std::uint64_t first = z ^ (z >> 31U);

  state_ += 0x9E3779B97F4A7C15ULL;
  z = state_;
  z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
  const std::uint64_t second = z ^ (z >> 31U);

  Uuid128::bytes_type bytes{};
  for (int index = 0; index < 8; ++index) {
    bytes[static_cast<std::size_t>(index)] =
        static_cast<std::uint8_t>((first >> (8U * static_cast<unsigned>(index))) & 0xFFU);
    bytes[static_cast<std::size_t>(index) + 8] =
        static_cast<std::uint8_t>((second >> (8U * static_cast<unsigned>(index))) & 0xFFU);
  }
  Uuid128 value(bytes);
  if (value.is_nil()) {
    value = Uuid128(Uuid128::bytes_type{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1});
  }
  return AuthorityToken::from_value(value);
}

bool AuthorityRegistry::has_shard(const ShardId& shard) const { return find(shard) != nullptr; }

std::vector<ShardId> AuthorityRegistry::shards() const {
  std::vector<ShardId> out;
  out.reserve(shards_.size());
  for (const auto& entry : shards_) {
    out.push_back(entry.first);
  }
  return out;
}

std::optional<EpochNumber> AuthorityRegistry::current_epoch(const ShardId& shard) const {
  const ShardAuthority* authority = find(shard);
  if (authority == nullptr) {
    return std::nullopt;
  }
  return authority->epoch;
}

std::optional<Generation> AuthorityRegistry::current_generation(const ShardId& shard) const {
  const ShardAuthority* authority = find(shard);
  if (authority == nullptr) {
    return std::nullopt;
  }
  return authority->generation;
}

AuthorityRegistry::ShardAuthority* AuthorityRegistry::find(const ShardId& shard) {
  const auto it = shards_.find(shard);
  return it == shards_.end() ? nullptr : &it->second;
}

const AuthorityRegistry::ShardAuthority* AuthorityRegistry::find(const ShardId& shard) const {
  const auto it = shards_.find(shard);
  return it == shards_.end() ? nullptr : &it->second;
}

std::optional<AuthorityLease> AuthorityRegistry::active_mutating(const ShardAuthority& authority) const {
  const TimestampMs now_ms = clock_->now_ms();
  for (const AuthorityLease& lease : authority.leases) {
    if (blocks_mutation_slot(lease, now_ms)) {
      return lease;
    }
  }
  return std::nullopt;
}

std::optional<FenceRecord> AuthorityRegistry::pending_fence(const ShardAuthority& authority) const {
  for (auto it = authority.fences.rbegin(); it != authority.fences.rend(); ++it) {
    if (!it->acknowledged) {
      const TimestampMs now_ms = clock_->now_ms();
      for (const AuthorityLease& lease : authority.leases) {
        if (lease.id == it->fenced_lease && effective_state(lease, now_ms) == LeaseState::Expired) {
          return std::nullopt;
        }
      }
      return *it;
    }
  }
  return std::nullopt;
}

void AuthorityRegistry::expire_in_place(ShardAuthority& authority, TimestampMs now_ms) {
  for (AuthorityLease& lease : authority.leases) {
    if ((lease.state == LeaseState::Active || lease.state == LeaseState::Fencing) &&
        now_ms >= lease.expires_at_ms) {
      lease.state = LeaseState::Expired;
    }
  }
}

AuthorityToken AuthorityRegistry::mint_token() {
  if (tokens_ != nullptr) {
    return tokens_->next_token();
  }
  static RandomTokenSource fallback;
  return fallback.next_token();
}

Status AuthorityRegistry::establish(const ShardId& shard, EpochNumber epoch) {
  if (!shard.is_valid()) {
    return invalid_argument("shard id must be valid");
  }
  if (!epoch.is_valid()) {
    return invalid_argument("epoch must be a positive integer");
  }
  if (find(shard) != nullptr) {
    return Status::error(ErrorCode::AlreadyExists, "shard authority already established",
                         Json::object({{"shard", Json(shard.str())}}));
  }
  ShardAuthority authority;
  authority.shard = shard;
  authority.epoch = epoch;
  authority.generation = Generation::from_value(1);
  shards_.emplace(shard, std::move(authority));
  return Status::success();
}

Result<AuthorityLease> AuthorityRegistry::grant(const GrantRequest& request) {
  ShardAuthority* authority = find(request.shard);
  if (authority == nullptr) {
    return Status::error(ErrorCode::NotFound, "shard authority is not established",
                         Json::object({{"shard", Json(request.shard.str())}}));
  }
  if (!request.holder.is_valid()) {
    return invalid_argument("lease holder incarnation must be valid");
  }
  if (request.ttl_ms == 0) {
    return invalid_argument("lease ttl must be positive");
  }
  if (request.ttl_ms > kMaxLeaseTtlMs) {
    return Status::error(ErrorCode::BoundsExceeded, "lease ttl exceeds the configured maximum",
                         Json::object({{"requested_ms", Json(request.ttl_ms)}, {"max_ms", Json(kMaxLeaseTtlMs)}}));
  }

  const TimestampMs now_ms = clock_->now_ms();
  expire_in_place(*authority, now_ms);

  if (request.mode == AuthorityMode::Mutating) {
    const std::optional<AuthorityLease> blocker = active_mutating(*authority);
    if (blocker.has_value()) {
      // A lease that is being fenced can never be renewed: doing so would
      // resurrect an incarnation the controller has already ordered to stop.
      if (blocker->holder == request.holder && effective_state(*blocker, now_ms) == LeaseState::Active) {
        // Renewal by the incumbent: same token and generation, later expiry.
        for (AuthorityLease& lease : authority->leases) {
          if (lease.id == blocker->id) {
            lease.expires_at_ms = now_ms + request.ttl_ms;
            lease.attestation_seq += 1;
            lease.attested = true;
            lease.reason = request.reason;
            return lease;
          }
        }
      }
      return Status::error(
          ErrorCode::AuthorityConflict,
          "shard already has a mutating authority; fence or expire the incumbent before granting",
          Json::object({{"shard", Json(request.shard.str())},
                {"current_holder", Json(blocker->holder.to_string())},
                {"current_state", Json(std::string(lease_state_name(effective_state(*blocker, now_ms))))},
                {"current_generation", Json(blocker->generation.value())},
                {"requested_holder", Json(request.holder.to_string())}}));
    }
  } else {
    for (AuthorityLease& lease : authority->leases) {
      if (lease.holder == request.holder && lease.mode == request.mode &&
          effective_state(lease, now_ms) == LeaseState::Active) {
        lease.expires_at_ms = now_ms + request.ttl_ms;
        lease.attestation_seq += 1;
        lease.attested = true;
        lease.reason = request.reason;
        return lease;
      }
    }
  }

  if (request.mode == AuthorityMode::ReadOnlyShared) {
    std::size_t read_only_count = 0;
    for (const AuthorityLease& lease : authority->leases) {
      if (lease.mode == AuthorityMode::ReadOnlyShared && effective_state(lease, now_ms) == LeaseState::Active) {
        ++read_only_count;
      }
    }
    if (read_only_count >= kMaxReadOnlyLeasesPerShard) {
      return Status::error(ErrorCode::ResourceExhausted,
                           "read-only shared authority lease limit reached for shard",
                           Json::object({{"shard", Json(request.shard.str())},
                                 {"limit", Json(static_cast<std::uint64_t>(kMaxReadOnlyLeasesPerShard))}}));
    }
  } else {
    // A new mutating holder is a new generation. Generation is the axis that
    // makes every claim minted by the previous holder permanently stale, and it
    // is bumped only when mutation authority changes hands so that a read-only
    // shared lease never invalidates the incumbent writer's claims.
    const auto next_generation = authority->generation.next();
    if (!next_generation.has_value()) {
      return Status::error(ErrorCode::BoundsExceeded, "generation counter exhausted for shard",
                           Json::object({{"shard", Json(request.shard.str())}}));
    }
    authority->generation = *next_generation;
  }

  const auto next_lease_id = authority->next_lease_id.next();
  if (!next_lease_id.has_value()) {
    return Status::error(ErrorCode::BoundsExceeded, "lease id space exhausted for shard",
                         Json::object({{"shard", Json(request.shard.str())}}));
  }

  AuthorityLease lease;
  lease.id = authority->next_lease_id;
  authority->next_lease_id = *next_lease_id;
  lease.shard = request.shard;
  lease.holder = request.holder;
  lease.epoch = authority->epoch;
  lease.generation = authority->generation;
  lease.token = mint_token();
  lease.mode = request.mode;
  lease.state = LeaseState::Active;
  lease.issued_at_ms = now_ms;
  lease.expires_at_ms = now_ms + request.ttl_ms;
  lease.attested = true;
  lease.attestation_seq = 1;
  lease.campaign = request.campaign;
  lease.handoff = request.handoff;
  lease.reason = request.reason;
  authority->leases.push_back(lease);
  if (authority->leases.size() > kMaxLeaseHistoryPerShard) {
    authority->leases.erase(authority->leases.begin());
  }
  return lease;
}

Status AuthorityRegistry::renew(const ShardId& shard, const LeaseId& lease_id, std::uint64_t ttl_ms) {
  ShardAuthority* authority = find(shard);
  if (authority == nullptr) {
    return Status::error(ErrorCode::NotFound, "shard authority is not established");
  }
  if (ttl_ms == 0 || ttl_ms > kMaxLeaseTtlMs) {
    return Status::error(ErrorCode::BoundsExceeded, "lease ttl outside permitted range");
  }
  const TimestampMs now_ms = clock_->now_ms();
  expire_in_place(*authority, now_ms);
  for (AuthorityLease& lease : authority->leases) {
    if (lease.id == lease_id) {
      if (lease.state != LeaseState::Active) {
        return Status::error(ErrorCode::StaleAuthority, "lease is no longer active",
                             Json::object({{"state", Json(std::string(lease_state_name(lease.state)))}}));
      }
      lease.expires_at_ms = now_ms + ttl_ms;
      lease.attestation_seq += 1;
      return Status::success();
    }
  }
  return Status::error(ErrorCode::NotFound, "lease not found");
}

Status AuthorityRegistry::request_fence(const ShardId& shard, const IncarnationId& target,
                                        std::string reason) {
  ShardAuthority* authority = find(shard);
  if (authority == nullptr) {
    return Status::error(ErrorCode::NotFound, "shard authority is not established");
  }
  if (!target.is_valid()) {
    return invalid_argument("fence target incarnation must be valid");
  }
  const TimestampMs now_ms = clock_->now_ms();
  expire_in_place(*authority, now_ms);

  for (const FenceRecord& fence : authority->fences) {
    if (!fence.acknowledged && fence.target == target) {
      // Idempotent: a duplicate fence request returns the outstanding fence.
      return Status::error(ErrorCode::DuplicateFrame, "fence already outstanding for this incarnation",
                           Json::object({{"fence_id", Json(fence.id.value())},
                                 {"generation", Json(fence.generation.value())}}));
    }
  }

  AuthorityLease* holder = nullptr;
  for (AuthorityLease& lease : authority->leases) {
    if (lease.holder == target && lease.mode == AuthorityMode::Mutating &&
        effective_state(lease, now_ms) == LeaseState::Active) {
      holder = &lease;
      break;
    }
  }
  if (holder == nullptr) {
    return Status::error(ErrorCode::NotFound, "no active mutating lease held by the fence target",
                         Json::object({{"shard", Json(shard.str())}, {"target", Json(target.to_string())}}));
  }

  const auto next_generation = authority->generation.next();
  if (!next_generation.has_value()) {
    return Status::error(ErrorCode::BoundsExceeded, "generation counter exhausted for shard");
  }
  const auto next_fence_id = authority->next_fence_id.next();
  if (!next_fence_id.has_value()) {
    return Status::error(ErrorCode::BoundsExceeded, "fence id space exhausted for shard");
  }
  authority->generation = *next_generation;

  FenceRecord fence;
  fence.id = authority->next_fence_id;
  authority->next_fence_id = *next_fence_id;
  fence.shard = shard;
  fence.target = target;
  fence.epoch = authority->epoch;
  fence.generation = authority->generation;
  fence.fenced_lease = holder->id;
  fence.issued_at_ms = now_ms;
  fence.reason = std::move(reason);
  holder->state = LeaseState::Fencing;
  authority->fences.push_back(fence);
  if (authority->fences.size() > kMaxFenceHistoryPerShard) {
    authority->fences.erase(authority->fences.begin());
  }
  return Status::success();
}

Status AuthorityRegistry::acknowledge_fence(const ShardId& shard, const IncarnationId& holder,
                                            const FenceId& fence_id) {
  ShardAuthority* authority = find(shard);
  if (authority == nullptr) {
    return Status::error(ErrorCode::NotFound, "shard authority is not established");
  }
  const TimestampMs now_ms = clock_->now_ms();
  for (FenceRecord& fence : authority->fences) {
    if (fence.id != fence_id) {
      continue;
    }
    if (fence.target != holder) {
      return Status::error(ErrorCode::StaleIncarnation,
                           "fence acknowledgement came from a different incarnation than the target",
                           Json::object({{"expected", Json(fence.target.to_string())},
                                 {"presented", Json(holder.to_string())}}));
    }
    if (fence.acknowledged) {
      return Status::success();
    }
    fence.acknowledged = true;
    fence.acknowledged_at_ms = now_ms;
    for (AuthorityLease& lease : authority->leases) {
      if (lease.id == fence.fenced_lease && lease.state == LeaseState::Fencing) {
        lease.state = LeaseState::Fenced;
      }
    }
    return Status::success();
  }
  return Status::error(ErrorCode::NotFound, "fence record not found",
                       Json::object({{"fence_id", Json(fence_id.value())}}));
}

Status AuthorityRegistry::revoke(const ShardId& shard, const LeaseId& lease_id, std::string reason) {
  ShardAuthority* authority = find(shard);
  if (authority == nullptr) {
    return Status::error(ErrorCode::NotFound, "shard authority is not established");
  }
  for (AuthorityLease& lease : authority->leases) {
    if (lease.id == lease_id) {
      lease.state = LeaseState::Revoked;
      lease.reason = std::move(reason);
      return Status::success();
    }
  }
  return Status::error(ErrorCode::NotFound, "lease not found");
}

Status AuthorityRegistry::expire_due() {
  const TimestampMs now_ms = clock_->now_ms();
  for (auto& entry : shards_) {
    expire_in_place(entry.second, now_ms);
  }
  return Status::success();
}

Status AuthorityRegistry::reattest(const ShardId& shard, const IncarnationId& holder,
                                   const AuthorityToken& token) {
  ShardAuthority* authority = find(shard);
  if (authority == nullptr) {
    return Status::error(ErrorCode::NotFound, "shard authority is not established");
  }
  const TimestampMs now_ms = clock_->now_ms();
  expire_in_place(*authority, now_ms);
  for (AuthorityLease& lease : authority->leases) {
    if (lease.holder == holder && lease.token == token) {
      if (lease.state != LeaseState::Active) {
        return Status::error(ErrorCode::StaleAuthority, "cannot re-attest a lease that is not active",
                             Json::object({{"state", Json(std::string(lease_state_name(lease.state)))}}));
      }
      if (now_ms >= lease.expires_at_ms) {
        return Status::error(ErrorCode::StaleAuthority, "cannot re-attest an expired lease",
                             Json::object({{"expired_at_ms", Json(lease.expires_at_ms)},
                                   {"now_ms", Json(now_ms)}}));
      }
      lease.attested = true;
      lease.attestation_seq += 1;
      return Status::success();
    }
  }
  return Status::error(ErrorCode::StaleAuthority,
                       "no lease matches the presented token for this incarnation",
                       Json::object({{"shard", Json(shard.str())}, {"holder", Json(holder.to_string())}}));
}

Status AuthorityRegistry::advance_epoch(const ShardId& shard, EpochNumber epoch) {
  ShardAuthority* authority = find(shard);
  if (authority == nullptr) {
    return Status::error(ErrorCode::NotFound, "shard authority is not established");
  }
  if (!epoch.is_valid() || epoch <= authority->epoch) {
    return Status::error(ErrorCode::StaleEpoch, "new epoch must be strictly greater than the current epoch",
                         Json::object({{"current", Json(authority->epoch.value())}, {"requested", Json(epoch.value())}}));
  }
  authority->epoch = epoch;
  authority->generation = Generation::from_value(1);
  for (AuthorityLease& lease : authority->leases) {
    if (lease.state == LeaseState::Active || lease.state == LeaseState::Fencing) {
      lease.state = LeaseState::Revoked;
      lease.reason = "revoked by epoch advance";
    }
  }
  return Status::success();
}

Status AuthorityRegistry::validate_claim(const AuthorityClaim& claim, AuthorityMode required) const {
  const ShardAuthority* authority = find(claim.shard);
  if (authority == nullptr) {
    return Status::error(ErrorCode::NotFound, "shard authority is not established",
                         Json::object({{"shard", Json(claim.shard.str())}}));
  }
  if (!claim.is_valid()) {
    return invalid_argument("authority claim is incomplete");
  }
  if (claim.epoch != authority->epoch) {
    return Status::error(ErrorCode::StaleEpoch, "authority claim carries a stale epoch",
                         Json::object({{"presented", Json(claim.epoch.value())},
                               {"current", Json(authority->epoch.value())}}));
  }
  if (claim.generation != authority->generation) {
    return Status::error(ErrorCode::StaleGeneration, "authority claim carries a stale generation",
                         Json::object({{"presented", Json(claim.generation.value())},
                               {"current", Json(authority->generation.value())}}));
  }
  const TimestampMs now_ms = clock_->now_ms();
  for (const AuthorityLease& lease : authority->leases) {
    if (lease.holder != claim.incarnation || lease.token != claim.token) {
      continue;
    }
    const LeaseState state = effective_state(lease, now_ms);
    if (state != LeaseState::Active) {
      return Status::error(ErrorCode::StaleAuthority, "authority lease is not active",
                           Json::object({{"state", Json(std::string(lease_state_name(state)))},
                                 {"holder", Json(claim.incarnation.to_string())}}));
    }
    if (!lease.attested) {
      return Status::error(ErrorCode::StaleAuthority,
                           "authority lease has not been re-attested since recovery",
                           Json::object({{"holder", Json(claim.incarnation.to_string())}}));
    }
    if (authority_allows_mutation(required) && !authority_allows_mutation(lease.mode)) {
      return Status::error(ErrorCode::NotAuthoritative, "lease does not permit mutation",
                           Json::object({{"lease_mode", Json(std::string(mode_name(lease.mode)))},
                                 {"required", Json(std::string(mode_name(required)))}}));
    }
    if (authority_allows_read(required) && !authority_allows_read(lease.mode)) {
      return Status::error(ErrorCode::NotAuthoritative, "lease does not permit reads",
                           Json::object({{"lease_mode", Json(std::string(mode_name(lease.mode)))}}));
    }
    return Status::success();
  }
  return Status::error(ErrorCode::StaleAuthority,
                       "no lease matches the presented authority token for this incarnation",
                       Json::object({{"shard", Json(claim.shard.str())},
                             {"holder", Json(claim.incarnation.to_string())},
                             {"generation", Json(claim.generation.value())}}));
}

Result<AuthorityView> AuthorityRegistry::view(const ShardId& shard) const {
  const ShardAuthority* authority = find(shard);
  if (authority == nullptr) {
    return Status::error(ErrorCode::NotFound, "shard authority is not established",
                         Json::object({{"shard", Json(shard.str())}}));
  }
  const TimestampMs now_ms = clock_->now_ms();
  AuthorityView out;
  out.shard = shard;
  out.epoch = authority->epoch;
  out.generation = authority->generation;
  for (const AuthorityLease& lease : authority->leases) {
    AuthorityLease copy = lease;
    copy.state = effective_state(lease, now_ms);
    if (copy.mode == AuthorityMode::Mutating && copy.state == LeaseState::Active) {
      out.mutating = copy;
      out.slot_holder = copy;
    } else if (copy.mode == AuthorityMode::Mutating && copy.state == LeaseState::Fencing) {
      out.slot_holder = copy;
    } else if (copy.mode == AuthorityMode::ReadOnlyShared && copy.state == LeaseState::Active) {
      out.read_only.push_back(copy);
    }
  }
  for (const FenceRecord& fence : authority->fences) {
    out.fences.push_back(fence);
  }
  return out;
}

std::vector<AuthorityView> AuthorityRegistry::all_views() const {
  std::vector<AuthorityView> out;
  for (const auto& entry : shards_) {
    auto view = this->view(entry.first);
    if (view.ok()) {
      out.push_back(view.take());
    }
  }
  return out;
}

Json to_json(const AuthorityLease& lease) {
  Json out = Json::object();
  out.set("id", Json(lease.id.value()));
  out.set("shard", Json(lease.shard.str()));
  out.set("holder", to_json(lease.holder));
  out.set("epoch", Json(lease.epoch.value()));
  out.set("generation", Json(lease.generation.value()));
  out.set("token", Json(lease.token.to_compact_string()));
  out.set("mode", Json(std::string(mode_name(lease.mode))));
  out.set("state", Json(std::string(lease_state_name(lease.state))));
  out.set("issued_at_ms", Json(lease.issued_at_ms));
  out.set("expires_at_ms", Json(lease.expires_at_ms));
  out.set("attested", Json(lease.attested));
  out.set("attestation_seq", Json(lease.attestation_seq));
  if (lease.campaign.is_valid()) {
    out.set("campaign", Json(lease.campaign.str()));
  }
  if (lease.handoff.is_valid()) {
    out.set("handoff", Json(lease.handoff.value()));
  }
  out.set("reason", Json(lease.reason));
  return out;
}

Result<AuthorityLease> authority_lease_from_json(const Json& value) {
  if (!value.is_object()) {
    return malformed("authority lease must be an object");
  }
  AuthorityLease lease;
  const auto id = json_u64(value, "id");
  if (!id.has_value() || *id == 0) {
    return malformed("authority lease id must be positive");
  }
  lease.id = LeaseId::from_value(*id);

  const auto shard_text = json_string(value, "shard");
  if (!shard_text.has_value()) {
    return malformed("authority lease is missing shard");
  }
  auto shard = ShardId::parse(*shard_text);
  if (!shard.has_value()) {
    return malformed("authority lease shard is invalid: " + *shard_text);
  }
  lease.shard = *shard;

  const Json* holder = value.find("holder");
  if (holder == nullptr) {
    return malformed("authority lease is missing holder");
  }
  auto incarnation = incarnation_from_json(*holder);
  if (!incarnation.ok()) {
    return incarnation.status();
  }
  lease.holder = incarnation.value();

  const auto epoch = json_u64(value, "epoch");
  if (!epoch.has_value() || *epoch == 0) {
    return malformed("authority lease epoch must be positive");
  }
  lease.epoch = EpochNumber::from_value(*epoch);

  const auto generation = json_u64(value, "generation");
  if (!generation.has_value() || *generation == 0) {
    return malformed("authority lease generation must be positive");
  }
  lease.generation = Generation::from_value(*generation);

  const auto token_text = json_string(value, "token");
  if (!token_text.has_value()) {
    return malformed("authority lease is missing token");
  }
  auto token = AuthorityToken::parse(*token_text);
  if (!token.has_value() || token->is_nil()) {
    return malformed("authority lease token is not a valid uuid");
  }
  lease.token = *token;

  const auto mode_text = json_string(value, "mode");
  if (!mode_text.has_value()) {
    return malformed("authority lease is missing mode");
  }
  const auto mode = mode_from_name(*mode_text);
  if (!mode.has_value()) {
    return malformed("authority lease mode is invalid: " + *mode_text);
  }
  lease.mode = *mode;

  const auto state_text = json_string(value, "state");
  if (!state_text.has_value()) {
    return malformed("authority lease is missing state");
  }
  const auto state = lease_state_from_name(*state_text);
  if (!state.has_value()) {
    return malformed("authority lease state is invalid: " + *state_text);
  }
  lease.state = *state;

  lease.issued_at_ms = json_u64_or(value, "issued_at_ms", 0);
  lease.expires_at_ms = json_u64_or(value, "expires_at_ms", 0);
  lease.attestation_seq = json_u64_or(value, "attestation_seq", 0);
  lease.attested = json_bool_or(value, "attested", false);
  if (const auto campaign = json_string(value, "campaign")) {
    auto parsed = CampaignId::parse(*campaign);
    if (!parsed.has_value()) {
      return malformed("authority lease campaign is invalid: " + *campaign);
    }
    lease.campaign = *parsed;
  }
  if (const auto handoff = json_u64(value, "handoff")) {
    lease.handoff = HandoffId::from_value(*handoff);
  }
  lease.reason = json_string_or(value, "reason", "");
  return lease;
}

Json to_json(const FenceRecord& fence) {
  Json out = Json::object();
  out.set("id", Json(fence.id.value()));
  out.set("shard", Json(fence.shard.str()));
  out.set("target", to_json(fence.target));
  out.set("epoch", Json(fence.epoch.value()));
  out.set("generation", Json(fence.generation.value()));
  out.set("fenced_lease", Json(fence.fenced_lease.value()));
  out.set("issued_at_ms", Json(fence.issued_at_ms));
  out.set("acknowledged_at_ms", Json(fence.acknowledged_at_ms));
  out.set("acknowledged", Json(fence.acknowledged));
  out.set("reason", Json(fence.reason));
  return out;
}

Result<FenceRecord> fence_record_from_json(const Json& value) {
  if (!value.is_object()) {
    return malformed("fence record must be an object");
  }
  FenceRecord fence;
  const auto id = json_u64(value, "id");
  if (!id.has_value() || *id == 0) {
    return malformed("fence record id must be positive");
  }
  fence.id = FenceId::from_value(*id);
  const auto shard_text = json_string(value, "shard");
  if (!shard_text.has_value()) {
    return malformed("fence record is missing shard");
  }
  auto shard = ShardId::parse(*shard_text);
  if (!shard.has_value()) {
    return malformed("fence record shard is invalid");
  }
  fence.shard = *shard;
  const Json* target = value.find("target");
  if (target == nullptr) {
    return malformed("fence record is missing target");
  }
  auto incarnation = incarnation_from_json(*target);
  if (!incarnation.ok()) {
    return incarnation.status();
  }
  fence.target = incarnation.value();
  const auto epoch = json_u64(value, "epoch");
  const auto generation = json_u64(value, "generation");
  if (!epoch.has_value() || *epoch == 0 || !generation.has_value() || *generation == 0) {
    return malformed("fence record epoch/generation must be positive");
  }
  fence.epoch = EpochNumber::from_value(*epoch);
  fence.generation = Generation::from_value(*generation);
  fence.fenced_lease = LeaseId::from_value(json_u64_or(value, "fenced_lease", 0));
  fence.issued_at_ms = json_u64_or(value, "issued_at_ms", 0);
  fence.acknowledged_at_ms = json_u64_or(value, "acknowledged_at_ms", 0);
  fence.acknowledged = json_bool_or(value, "acknowledged", false);
  fence.reason = json_string_or(value, "reason", "");
  return fence;
}

Json AuthorityRegistry::to_json() const {
  Json out = Json::object();
  out.set("next_lease_id", Json(next_lease_id_.value()));
  out.set("next_fence_id", Json(next_fence_id_.value()));
  Json shards = Json::array();
  for (const auto& entry : shards_) {
    const ShardAuthority& authority = entry.second;
    Json shard = Json::object();
    shard.set("shard", Json(authority.shard.str()));
    shard.set("epoch", Json(authority.epoch.value()));
    shard.set("generation", Json(authority.generation.value()));
    shard.set("next_lease_id", Json(authority.next_lease_id.value()));
    shard.set("next_fence_id", Json(authority.next_fence_id.value()));
    Json leases = Json::array();
    for (const AuthorityLease& lease : authority.leases) {
      leases.push_back(::fabric::evolution::to_json(lease));
    }
    shard.set("leases", std::move(leases));
    Json fences = Json::array();
    for (const FenceRecord& fence : authority.fences) {
      fences.push_back(::fabric::evolution::to_json(fence));
    }
    shard.set("fences", std::move(fences));
    shards.push_back(std::move(shard));
  }
  out.set("shards", std::move(shards));
  return out;
}

Status AuthorityRegistry::load(const Json& value) {
  if (!value.is_object()) {
    return malformed("authority registry snapshot must be an object");
  }
  const Json* shards = value.find("shards");
  if (shards == nullptr || !shards->is_array()) {
    return malformed("authority registry snapshot is missing the shards array");
  }
  std::map<ShardId, ShardAuthority> restored;
  for (std::size_t index = 0; index < shards->size(); ++index) {
    const Json& shard = shards->at(index);
    if (!shard.is_object()) {
      return malformed("authority registry shard entry must be an object");
    }
    const auto shard_text = json_string(shard, "shard");
    if (!shard_text.has_value()) {
      return malformed("authority registry shard entry is missing shard");
    }
    auto shard_id = ShardId::parse(*shard_text);
    if (!shard_id.has_value()) {
      return malformed("authority registry shard entry has an invalid shard id");
    }
    const auto epoch = json_u64(shard, "epoch");
    const auto generation = json_u64(shard, "generation");
    if (!epoch.has_value() || *epoch == 0 || !generation.has_value() || *generation == 0) {
      return malformed("authority registry shard entry has an invalid epoch or generation");
    }
    ShardAuthority authority;
    authority.shard = *shard_id;
    authority.epoch = EpochNumber::from_value(*epoch);
    authority.generation = Generation::from_value(*generation);
    authority.next_lease_id = LeaseId::from_value(json_u64_or(shard, "next_lease_id", 1));
    authority.next_fence_id = FenceId::from_value(json_u64_or(shard, "next_fence_id", 1));
    if (const Json* leases = shard.find("leases")) {
      if (!leases->is_array()) {
        return malformed("authority registry leases must be an array");
      }
      for (std::size_t lease_index = 0; lease_index < leases->size(); ++lease_index) {
        auto lease = authority_lease_from_json(leases->at(lease_index));
        if (!lease.ok()) {
          return lease.status();
        }
        AuthorityLease restored_lease = lease.take();
        if (restored_lease.shard != authority.shard) {
          return Status::error(ErrorCode::IntegrityFailure,
                               "authority lease shard does not match its containing shard entry");
        }
        // Persisted leases never come back live. They must be re-attested by the
        // holder before they can authorise anything again.
        restored_lease.attested = false;
        authority.leases.push_back(restored_lease);
      }
    }
    if (const Json* fences = shard.find("fences")) {
      if (!fences->is_array()) {
        return malformed("authority registry fences must be an array");
      }
      for (std::size_t fence_index = 0; fence_index < fences->size(); ++fence_index) {
        auto fence = fence_record_from_json(fences->at(fence_index));
        if (!fence.ok()) {
          return fence.status();
        }
        authority.fences.push_back(fence.take());
      }
    }
    if (restored.find(authority.shard) != restored.end()) {
      return Status::error(ErrorCode::IntegrityFailure, "duplicate shard entry in authority registry");
    }
    restored.emplace(authority.shard, std::move(authority));
  }
  shards_ = std::move(restored);
  next_lease_id_ = LeaseId::from_value(json_u64_or(value, "next_lease_id", 1));
  next_fence_id_ = FenceId::from_value(json_u64_or(value, "next_fence_id", 1));
  return Status::success();
}

void AuthorityRegistry::clear() { shards_.clear(); }

}  // namespace fabric::evolution
