// Fabric Evolution — authority registry tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <string>
#include <vector>

#include "fabric/evolution/authority.hpp"
#include "fabric/evolution/clock.hpp"
#include "support/test_harness.hpp"

namespace fabric::evolution {
namespace {

IncarnationId make_incarnation(const char* name, std::uint64_t boot, std::uint64_t uuid_seed) {
  const auto component = ComponentId::parse(name);
  std::uint8_t bytes[16] = {};
  for (int index = 0; index < 8; ++index) {
    bytes[index] = static_cast<std::uint8_t>((uuid_seed >> (8U * static_cast<unsigned>(index))) & 0xFFU);
    bytes[index + 8] = static_cast<std::uint8_t>((boot >> (8U * static_cast<unsigned>(index))) & 0xFFU);
  }
  Uuid128::bytes_type raw{};
  for (std::size_t index = 0; index < raw.size(); ++index) {
    raw[index] = bytes[index];
  }
  return IncarnationId(*component, IncarnationUuid::from_value(Uuid128(raw)), BootCounter::from_value(boot));
}

ShardId shard_zero() { return *ShardId::parse("shard-0"); }

struct Fixture {
  ManualClock clock;
  SeededTokenSource tokens{42};
  AuthorityRegistry registry{clock};

  Fixture() {
    registry.set_token_source(&tokens);
    FABRIC_CHECK_OK(registry.establish(shard_zero(), EpochNumber::from_value(1)));
  }
};

}  // namespace

FABRIC_TEST(authority, establish_grants_and_validates) {
  Fixture fixture;
  const IncarnationId holder = make_incarnation("node-a", 1, 11);
  GrantRequest request;
  request.shard = shard_zero();
  request.holder = holder;
  request.ttl_ms = 1000;
  request.reason = "initial authority";
  auto lease = fixture.registry.grant(request);
  FABRIC_CHECK_OK(lease);

  AuthorityClaim claim;
  claim.shard = shard_zero();
  claim.incarnation = holder;
  claim.epoch = lease.value().epoch;
  claim.generation = lease.value().generation;
  claim.token = lease.value().token;
  FABRIC_CHECK_OK(fixture.registry.validate_claim(claim, AuthorityMode::Mutating));

  // The lease is bound to its holder: another incarnation cannot borrow it.
  AuthorityClaim impostor = claim;
  impostor.incarnation = make_incarnation("node-b", 1, 12);
  FABRIC_CHECK_ERR(fixture.registry.validate_claim(impostor, AuthorityMode::Mutating),
                   ErrorCode::StaleAuthority);

  // Unknown shard is not implicitly authoritative.
  AuthorityClaim unknown = claim;
  unknown.shard = *ShardId::parse("shard-9");
  FABRIC_CHECK_ERR(fixture.registry.validate_claim(unknown, AuthorityMode::Mutating), ErrorCode::NotFound);
}

FABRIC_TEST(authority, establish_is_idempotent_checked) {
  Fixture fixture;
  FABRIC_CHECK_ERR(fixture.registry.establish(shard_zero(), EpochNumber::from_value(2)),
                   ErrorCode::AlreadyExists);
  FABRIC_CHECK_ERR(fixture.registry.establish(shard_zero(), EpochNumber::invalid()),
                   ErrorCode::InvalidArgument);
}

FABRIC_TEST(authority, single_mutating_owner_is_enforced) {
  Fixture fixture;
  GrantRequest request;
  request.shard = shard_zero();
  request.holder = make_incarnation("node-a", 1, 11);
  request.ttl_ms = 1000;
  FABRIC_CHECK_OK(fixture.registry.grant(request));

  GrantRequest second = request;
  second.holder = make_incarnation("node-b", 1, 12);
  FABRIC_CHECK_ERR(fixture.registry.grant(second), ErrorCode::AuthorityConflict);

  const auto view = fixture.registry.view(shard_zero());
  FABRIC_CHECK_OK(view);
  FABRIC_CHECK_EQ(view.value().mutating_authority_count(), static_cast<std::size_t>(1));
}

FABRIC_TEST(authority, renewal_keeps_generation_and_token) {
  Fixture fixture;
  GrantRequest request;
  request.shard = shard_zero();
  request.holder = make_incarnation("node-a", 1, 11);
  request.ttl_ms = 1000;
  auto first = fixture.registry.grant(request);
  FABRIC_CHECK_OK(first);

  fixture.clock.advance(500);
  auto renewed = fixture.registry.grant(request);
  FABRIC_CHECK_OK(renewed);
  FABRIC_CHECK_EQ(renewed.value().generation, first.value().generation);
  FABRIC_CHECK_EQ(renewed.value().token, first.value().token);
  FABRIC_CHECK_EQ(renewed.value().id, first.value().id);
  FABRIC_CHECK(renewed.value().expires_at_ms > first.value().expires_at_ms);
}

FABRIC_TEST(authority, fence_blocks_new_authority_until_acknowledged) {
  Fixture fixture;
  const IncarnationId predecessor = make_incarnation("node-a", 1, 11);
  const IncarnationId successor = make_incarnation("node-b", 1, 12);
  GrantRequest request;
  request.shard = shard_zero();
  request.holder = predecessor;
  request.ttl_ms = 100000;
  auto lease = fixture.registry.grant(request);
  FABRIC_CHECK_OK(lease);

  FABRIC_CHECK_OK(fixture.registry.request_fence(shard_zero(), predecessor, "handoff"));
  // A duplicate fence request for the same incarnation is recognised as such.
  FABRIC_CHECK_ERR(fixture.registry.request_fence(shard_zero(), predecessor, "handoff"),
                   ErrorCode::DuplicateFrame);

  // The successor cannot take authority while the predecessor's fence is
  // outstanding: this is what prevents two conflicting authoritative owners.
  GrantRequest successor_request = request;
  successor_request.holder = successor;
  FABRIC_CHECK_ERR(fixture.registry.grant(successor_request), ErrorCode::AuthorityConflict);

  // The predecessor's own claims are already stale: the fence bumped generation.
  AuthorityClaim stale_claim;
  stale_claim.shard = shard_zero();
  stale_claim.incarnation = predecessor;
  stale_claim.epoch = lease.value().epoch;
  stale_claim.generation = lease.value().generation;
  stale_claim.token = lease.value().token;
  FABRIC_CHECK_ERR(fixture.registry.validate_claim(stale_claim, AuthorityMode::Mutating),
                   ErrorCode::StaleGeneration);

  const auto view = fixture.registry.view(shard_zero());
  FABRIC_CHECK_OK(view);
  FABRIC_CHECK_EQ(view.value().mutating_authority_count(), static_cast<std::size_t>(0));
  FABRIC_CHECK_EQ(view.value().fences.size(), static_cast<std::size_t>(1));
  FABRIC_CHECK(view.value().generation > lease.value().generation);

  // Acknowledging from the wrong incarnation is refused.
  FABRIC_CHECK_ERR(fixture.registry.acknowledge_fence(shard_zero(), successor, view.value().fences[0].id),
                   ErrorCode::StaleIncarnation);
  FABRIC_CHECK_OK(fixture.registry.acknowledge_fence(shard_zero(), predecessor, view.value().fences[0].id));
  // Acknowledgement is idempotent.
  FABRIC_CHECK_OK(fixture.registry.acknowledge_fence(shard_zero(), predecessor, view.value().fences[0].id));

  auto successor_lease = fixture.registry.grant(successor_request);
  FABRIC_CHECK_OK(successor_lease);
  FABRIC_CHECK(successor_lease.value().generation > lease.value().generation);

  AuthorityClaim successor_claim;
  successor_claim.shard = shard_zero();
  successor_claim.incarnation = successor;
  successor_claim.epoch = successor_lease.value().epoch;
  successor_claim.generation = successor_lease.value().generation;
  successor_claim.token = successor_lease.value().token;
  FABRIC_CHECK_OK(fixture.registry.validate_claim(successor_claim, AuthorityMode::Mutating));

  // Replaying predecessor credentials against the new generation is refused.
  AuthorityClaim replay = stale_claim;
  replay.token = successor_lease.value().token;
  FABRIC_CHECK_ERR(fixture.registry.validate_claim(replay, AuthorityMode::Mutating),
                   ErrorCode::StaleGeneration);
}

FABRIC_TEST(authority, expiry_frees_the_mutation_slot) {
  Fixture fixture;
  const IncarnationId predecessor = make_incarnation("node-a", 1, 11);
  GrantRequest request;
  request.shard = shard_zero();
  request.holder = predecessor;
  request.ttl_ms = 1000;
  auto lease = fixture.registry.grant(request);
  FABRIC_CHECK_OK(lease);

  AuthorityClaim claim;
  claim.shard = shard_zero();
  claim.incarnation = predecessor;
  claim.epoch = lease.value().epoch;
  claim.generation = lease.value().generation;
  claim.token = lease.value().token;
  FABRIC_CHECK_OK(fixture.registry.validate_claim(claim, AuthorityMode::Mutating));

  fixture.clock.advance(1000);
  // An expired lease stops authorising immediately, even before any housekeeping.
  FABRIC_CHECK_ERR(fixture.registry.validate_claim(claim, AuthorityMode::Mutating),
                   ErrorCode::StaleAuthority);
  const auto view = fixture.registry.view(shard_zero());
  FABRIC_CHECK_OK(view);
  FABRIC_CHECK_EQ(view.value().mutating_authority_count(), static_cast<std::size_t>(0));

  GrantRequest successor = request;
  successor.holder = make_incarnation("node-b", 1, 12);
  FABRIC_CHECK_OK(fixture.registry.grant(successor));

  FABRIC_CHECK_OK(fixture.registry.expire_due());
  const auto after = fixture.registry.view(shard_zero());
  FABRIC_CHECK_OK(after);
  FABRIC_CHECK_EQ(after.value().mutating.value().holder, successor.holder);
}

FABRIC_TEST(authority, read_only_shared_phase_is_concurrent_and_non_mutating) {
  Fixture fixture;
  const IncarnationId writer = make_incarnation("node-a", 1, 11);
  GrantRequest write_request;
  write_request.shard = shard_zero();
  write_request.holder = writer;
  write_request.ttl_ms = 5000;
  auto write_lease = fixture.registry.grant(write_request);
  FABRIC_CHECK_OK(write_lease);

  GrantRequest read_request;
  read_request.shard = shard_zero();
  read_request.mode = AuthorityMode::ReadOnlyShared;
  read_request.ttl_ms = 5000;
  read_request.holder = make_incarnation("node-b", 1, 12);
  auto reader_one = fixture.registry.grant(read_request);
  FABRIC_CHECK_OK(reader_one);
  read_request.holder = make_incarnation("node-c", 1, 13);
  auto reader_two = fixture.registry.grant(read_request);
  FABRIC_CHECK_OK(reader_two);

  // Read-only grants must not invalidate the incumbent writer's claims.
  AuthorityClaim write_claim;
  write_claim.shard = shard_zero();
  write_claim.incarnation = writer;
  write_claim.epoch = write_lease.value().epoch;
  write_claim.generation = write_lease.value().generation;
  write_claim.token = write_lease.value().token;
  FABRIC_CHECK_OK(fixture.registry.validate_claim(write_claim, AuthorityMode::Mutating));

  const auto view = fixture.registry.view(shard_zero());
  FABRIC_CHECK_OK(view);
  FABRIC_CHECK_EQ(view.value().mutating_authority_count(), static_cast<std::size_t>(1));
  FABRIC_CHECK_EQ(view.value().read_only.size(), static_cast<std::size_t>(2));

  // A read-only lease may read but never mutate.
  AuthorityClaim read_claim;
  read_claim.shard = shard_zero();
  read_claim.incarnation = reader_one.value().holder;
  read_claim.epoch = reader_one.value().epoch;
  read_claim.generation = reader_one.value().generation;
  read_claim.token = reader_one.value().token;
  FABRIC_CHECK_OK(fixture.registry.validate_claim(read_claim, AuthorityMode::ReadOnlyShared));
  FABRIC_CHECK_ERR(fixture.registry.validate_claim(read_claim, AuthorityMode::Mutating),
                   ErrorCode::NotAuthoritative);
}

FABRIC_TEST(authority, a_fencing_lease_can_never_be_renewed) {
  Fixture fixture;
  const IncarnationId holder = make_incarnation("node-a", 1, 11);
  GrantRequest request;
  request.shard = shard_zero();
  request.holder = holder;
  request.ttl_ms = 100000;
  auto lease = fixture.registry.grant(request);
  FABRIC_CHECK_OK(lease);
  const Generation before = lease.value().generation;

  FABRIC_CHECK_OK(fixture.registry.request_fence(shard_zero(), holder, "test fence"));
  const auto fenced_view = fixture.registry.view(shard_zero());
  FABRIC_CHECK_OK(fenced_view);
  const Generation fenced_generation = fenced_view.value().generation;
  FABRIC_CHECK(fenced_generation > before);

  // Renewing the incumbent must be refused while the fence is outstanding.
  FABRIC_CHECK_ERR(fixture.registry.grant(request), ErrorCode::AuthorityConflict);
  const auto after = fixture.registry.view(shard_zero());
  FABRIC_CHECK_OK(after);
  FABRIC_CHECK_EQ(after.value().generation, fenced_generation);
  FABRIC_CHECK_EQ(after.value().mutating_authority_count(), static_cast<std::size_t>(0));
  FABRIC_CHECK_EQ(after.value().slot_holder.value().state, LeaseState::Fencing);

  // Once the fence is acknowledged the incumbent still cannot renew its old
  // lease; authority must be granted explicitly as a new generation.
  FABRIC_CHECK_OK(
      fixture.registry.acknowledge_fence(shard_zero(), holder, fenced_view.value().fences[0].id));
  auto re_granted = fixture.registry.grant(request);
  FABRIC_CHECK_OK(re_granted);
  FABRIC_CHECK(re_granted.value().generation > fenced_generation);
  FABRIC_CHECK_NE(re_granted.value().token, lease.value().token);
}

FABRIC_TEST(authority, read_only_lease_limit_is_enforced) {
  Fixture fixture;
  GrantRequest read_request;
  read_request.shard = shard_zero();
  read_request.mode = AuthorityMode::ReadOnlyShared;
  read_request.ttl_ms = 5000;
  for (std::uint64_t index = 0; index < 8; ++index) {
    read_request.holder = make_incarnation("node-a", index + 1, 100 + index);
    FABRIC_CHECK_OK(fixture.registry.grant(read_request));
  }
  read_request.holder = make_incarnation("node-a", 9, 109);
  FABRIC_CHECK_ERR(fixture.registry.grant(read_request), ErrorCode::ResourceExhausted);
}

FABRIC_TEST(authority, epoch_advance_revokes_every_lease) {
  Fixture fixture;
  GrantRequest request;
  request.shard = shard_zero();
  request.holder = make_incarnation("node-a", 1, 11);
  request.ttl_ms = 5000;
  auto lease = fixture.registry.grant(request);
  FABRIC_CHECK_OK(lease);

  FABRIC_CHECK_ERR(fixture.registry.advance_epoch(shard_zero(), EpochNumber::from_value(1)),
                   ErrorCode::StaleEpoch);
  FABRIC_CHECK_OK(fixture.registry.advance_epoch(shard_zero(), EpochNumber::from_value(2)));

  AuthorityClaim claim;
  claim.shard = shard_zero();
  claim.incarnation = request.holder;
  claim.epoch = lease.value().epoch;
  claim.generation = lease.value().generation;
  claim.token = lease.value().token;
  FABRIC_CHECK_ERR(fixture.registry.validate_claim(claim, AuthorityMode::Mutating),
                   ErrorCode::StaleEpoch);

  const auto view = fixture.registry.view(shard_zero());
  FABRIC_CHECK_OK(view);
  FABRIC_CHECK_EQ(view.value().epoch, EpochNumber::from_value(2));
  FABRIC_CHECK_EQ(view.value().generation, Generation::from_value(1));
  FABRIC_CHECK_EQ(view.value().mutating_authority_count(), static_cast<std::size_t>(0));
}

FABRIC_TEST(authority, restored_leases_require_reattestation) {
  Fixture fixture;
  const IncarnationId holder = make_incarnation("node-a", 1, 11);
  GrantRequest request;
  request.shard = shard_zero();
  request.holder = holder;
  request.ttl_ms = 100000;
  auto lease = fixture.registry.grant(request);
  FABRIC_CHECK_OK(lease);
  const Json snapshot = fixture.registry.to_json();

  ManualClock restarted_clock(fixture.clock.now_ms());
  AuthorityRegistry restored(restarted_clock);
  FABRIC_CHECK_OK(restored.load(snapshot));

  AuthorityClaim claim;
  claim.shard = shard_zero();
  claim.incarnation = holder;
  claim.epoch = lease.value().epoch;
  claim.generation = lease.value().generation;
  claim.token = lease.value().token;
  // Deserialisation alone never makes an authority current again.
  FABRIC_CHECK_ERR(restored.validate_claim(claim, AuthorityMode::Mutating), ErrorCode::StaleAuthority);

  // A token the registry never issued cannot re-attest anything.
  SeededTokenSource unrelated_tokens(999);
  FABRIC_CHECK_ERR(restored.reattest(shard_zero(), holder, unrelated_tokens.next_token()),
                   ErrorCode::StaleAuthority);
  FABRIC_CHECK_ERR(restored.reattest(shard_zero(), make_incarnation("node-z", 1, 77), lease.value().token),
                   ErrorCode::StaleAuthority);
  FABRIC_CHECK_OK(restored.reattest(shard_zero(), holder, lease.value().token));
  FABRIC_CHECK_OK(restored.validate_claim(claim, AuthorityMode::Mutating));
}

FABRIC_TEST(authority, reattestation_is_impossible_after_fence) {
  Fixture fixture;
  const IncarnationId holder = make_incarnation("node-a", 1, 11);
  GrantRequest request;
  request.shard = shard_zero();
  request.holder = holder;
  request.ttl_ms = 100000;
  auto lease = fixture.registry.grant(request);
  FABRIC_CHECK_OK(lease);
  const Json snapshot = fixture.registry.to_json();

  ManualClock restarted_clock(fixture.clock.now_ms());
  AuthorityRegistry restored(restarted_clock);
  FABRIC_CHECK_OK(restored.load(snapshot));
  FABRIC_CHECK_OK(restored.request_fence(shard_zero(), holder, "recovered fence"));
  FABRIC_CHECK_ERR(restored.reattest(shard_zero(), holder, lease.value().token),
                   ErrorCode::StaleAuthority);
}

FABRIC_TEST(authority, registry_persistence_round_trip) {
  Fixture fixture;
  GrantRequest request;
  request.shard = shard_zero();
  request.holder = make_incarnation("node-a", 1, 11);
  request.ttl_ms = 5000;
  request.reason = "initial";
  auto lease = fixture.registry.grant(request);
  FABRIC_CHECK_OK(lease);
  FABRIC_CHECK_OK(fixture.registry.establish(*ShardId::parse("shard-1"), EpochNumber::from_value(4)));

  const Json snapshot = fixture.registry.to_json();
  ManualClock other_clock(fixture.clock.now_ms());
  AuthorityRegistry restored(other_clock);
  FABRIC_CHECK_OK(restored.load(snapshot));

  const auto view = restored.view(shard_zero());
  FABRIC_CHECK_OK(view);
  FABRIC_CHECK_EQ(view.value().epoch, EpochNumber::from_value(1));
  FABRIC_CHECK_EQ(view.value().mutating.value().token, lease.value().token);
  FABRIC_CHECK_EQ(view.value().mutating.value().attested, false);
  FABRIC_CHECK_EQ(restored.current_epoch(*ShardId::parse("shard-1")).value(), EpochNumber::from_value(4));
  FABRIC_CHECK_EQ(restored.shards().size(), static_cast<std::size_t>(2));
}

FABRIC_TEST(authority, registry_rejects_malformed_snapshots) {
  ManualClock clock;
  AuthorityRegistry registry(clock);
  FABRIC_CHECK_ERR(registry.load(Json::array()), ErrorCode::Malformed);
  FABRIC_CHECK_ERR(registry.load(Json::object()), ErrorCode::Malformed);
  FABRIC_CHECK_ERR(registry.load(Json::object({{"shards", Json(1)}})), ErrorCode::Malformed);

  Json bad_shard = Json::object();
  Json shards = Json::array();
  shards.push_back(Json::object({{"shard", Json("shard-0")}, {"epoch", Json(0)}, {"generation", Json(1)}}));
  bad_shard.set("shards", std::move(shards));
  FABRIC_CHECK_ERR(registry.load(bad_shard), ErrorCode::Malformed);

  Json duplicate = Json::object();
  Json entries = Json::array();
  entries.push_back(Json::object({{"shard", Json("shard-0")}, {"epoch", Json(1)}, {"generation", Json(1)}}));
  entries.push_back(Json::object({{"shard", Json("shard-0")}, {"epoch", Json(2)}, {"generation", Json(1)}}));
  duplicate.set("shards", std::move(entries));
  FABRIC_CHECK_ERR(registry.load(duplicate), ErrorCode::IntegrityFailure);
}

FABRIC_TEST(authority, lease_ttl_bounds_are_checked) {
  Fixture fixture;
  GrantRequest request;
  request.shard = shard_zero();
  request.holder = make_incarnation("node-a", 1, 11);
  request.ttl_ms = 0;
  FABRIC_CHECK_ERR(fixture.registry.grant(request), ErrorCode::InvalidArgument);
  request.ttl_ms = 24ULL * 60ULL * 60ULL * 1000ULL + 1ULL;
  FABRIC_CHECK_ERR(fixture.registry.grant(request), ErrorCode::BoundsExceeded);
  request.ttl_ms = 1000;
  FABRIC_CHECK_OK(fixture.registry.grant(request));
}

FABRIC_TEST(authority, token_source_is_deterministic_when_seeded) {
  SeededTokenSource first_source(7);
  SeededTokenSource second_source(7);
  for (int index = 0; index < 64; ++index) {
    FABRIC_CHECK_EQ(first_source.next_token(), second_source.next_token());
  }
  SeededTokenSource other_source(8);
  SeededTokenSource reference(7);
  FABRIC_CHECK_NE(other_source.next_token(), reference.next_token());
}

FABRIC_TEST(authority, grant_for_unknown_shard_is_refused) {
  Fixture fixture;
  GrantRequest request;
  request.shard = *ShardId::parse("shard-42");
  request.holder = make_incarnation("node-a", 1, 11);
  request.ttl_ms = 1000;
  FABRIC_CHECK_ERR(fixture.registry.grant(request), ErrorCode::NotFound);
  FABRIC_CHECK_ERR(fixture.registry.view(request.shard), ErrorCode::NotFound);
  FABRIC_CHECK_ERR(fixture.registry.request_fence(request.shard, request.holder, "x"), ErrorCode::NotFound);
  FABRIC_CHECK_ERR(fixture.registry.acknowledge_fence(request.shard, request.holder, FenceId::from_value(1)),
                   ErrorCode::NotFound);
}

FABRIC_TEST(authority, revoke_and_revoke_unknown) {
  Fixture fixture;
  GrantRequest request;
  request.shard = shard_zero();
  request.holder = make_incarnation("node-a", 1, 11);
  request.ttl_ms = 1000;
  auto lease = fixture.registry.grant(request);
  FABRIC_CHECK_OK(lease);
  FABRIC_CHECK_OK(fixture.registry.revoke(shard_zero(), lease.value().id, "test"));
  FABRIC_CHECK_ERR(fixture.registry.revoke(shard_zero(), LeaseId::from_value(999), "test"),
                   ErrorCode::NotFound);
  GrantRequest successor = request;
  successor.holder = make_incarnation("node-b", 1, 12);
  FABRIC_CHECK_OK(fixture.registry.grant(successor));
}

FABRIC_TEST(authority, renew_validates_state_and_bounds) {
  Fixture fixture;
  GrantRequest request;
  request.shard = shard_zero();
  request.holder = make_incarnation("node-a", 1, 11);
  request.ttl_ms = 1000;
  auto lease = fixture.registry.grant(request);
  FABRIC_CHECK_OK(lease);
  FABRIC_CHECK_OK(fixture.registry.renew(shard_zero(), lease.value().id, 2000));
  FABRIC_CHECK_ERR(fixture.registry.renew(shard_zero(), lease.value().id, 0), ErrorCode::BoundsExceeded);
  FABRIC_CHECK_ERR(fixture.registry.renew(shard_zero(), LeaseId::from_value(4242), 100),
                   ErrorCode::NotFound);
  fixture.clock.advance(2001);
  FABRIC_CHECK_ERR(fixture.registry.renew(shard_zero(), lease.value().id, 100),
                   ErrorCode::StaleAuthority);
}

}  // namespace fabric::evolution
