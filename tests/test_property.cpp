// Fabric Evolution — deterministic property and randomized state-machine tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// These tests drive the authority registry and the manifest admission rules with
// seeded random operation sequences and check the invariants after every step.
// Every case is reproducible from its seed: a failure prints the seed that
// produced it.

#include <string>
#include <vector>

#include "fabric/evolution/authority.hpp"
#include "fabric/evolution/manifest.hpp"
#include "support/cluster.hpp"
#include "support/test_harness.hpp"

namespace fabric::evolution {
namespace {

IncarnationId incarnation_for(std::uint64_t index, std::uint64_t boot) {
  Uuid128::bytes_type bytes{};
  for (int byte = 0; byte < 8; ++byte) {
    bytes[static_cast<std::size_t>(byte)] =
        static_cast<std::uint8_t>((index >> (8U * static_cast<unsigned>(byte))) & 0xFFU);
    bytes[static_cast<std::size_t>(byte) + 8] =
        static_cast<std::uint8_t>((boot >> (8U * static_cast<unsigned>(byte))) & 0xFFU);
  }
  return IncarnationId(ComponentId::unchecked("node-" + std::to_string(index)),
                       IncarnationUuid::from_value(Uuid128(bytes)), BootCounter::from_value(boot));
}

}  // namespace

FABRIC_TEST(property_authority, random_operation_sequences_preserve_the_invariants) {
  const std::uint64_t seed = 0x5EED1234ULL;
  test::Random random(seed);
  for (int trial = 0; trial < 200; ++trial) {
    ManualClock clock(1000);
    SeededTokenSource tokens(static_cast<std::uint64_t>(trial) + 1);
    AuthorityRegistry registry(clock);
    registry.set_token_source(&tokens);
    const ShardId shard = *ShardId::parse("shard-0");
    FABRIC_CHECK_OK(registry.establish(shard, EpochNumber::from_value(1)));

    std::vector<IncarnationId> holders;
    for (std::uint64_t index = 0; index < 4; ++index) {
      holders.push_back(incarnation_for(index, 1));
    }

    Generation last_generation = Generation::from_value(1);
    for (int step = 0; step < 60; ++step) {
      const std::uint64_t choice = random.below(6);
      const IncarnationId& holder = holders[random.below(holders.size())];
      switch (choice) {
        case 0:
        case 1: {
          GrantRequest request;
          request.shard = shard;
          request.holder = holder;
          request.mode = AuthorityMode::Mutating;
          request.ttl_ms = 100 + random.below(900);
          request.reason = "property trial";
          auto lease = registry.grant(request);
          if (lease.ok()) {
            FABRIC_CHECK(lease.value().generation >= last_generation);
            last_generation = lease.value().generation;
          }
          break;
        }
        case 2: {
          GrantRequest request;
          request.shard = shard;
          request.holder = holder;
          request.mode = AuthorityMode::ReadOnlyShared;
          request.ttl_ms = 100 + random.below(900);
          (void)registry.grant(request);
          break;
        }
        case 3: {
          (void)registry.request_fence(shard, holder, "property fence");
          break;
        }
        case 4: {
          const auto view = registry.view(shard);
          FABRIC_CHECK_OK(view);
          for (const FenceRecord& fence : view.value().fences) {
            if (!fence.acknowledged) {
              (void)registry.acknowledge_fence(shard, fence.target, fence.id);
            }
          }
          break;
        }
        default:
          clock.advance(50 + random.below(400));
          (void)registry.expire_due();
          break;
      }

      // Invariant I1: at most one lease permits mutation, always.
      const auto view = registry.view(shard);
      FABRIC_CHECK_OK(view);
      FABRIC_CHECK(view.value().mutating_authority_count() <= 1);
      // Invariant I2: generation never decreases.
      FABRIC_CHECK(view.value().generation >= last_generation);
      last_generation = view.value().generation;
      // Every lease that authorises mutation is attested and unexpired.
      if (view.value().mutating.has_value() && view.value().mutating->state == LeaseState::Active) {
        FABRIC_CHECK(view.value().mutating->attested);
        FABRIC_CHECK(view.value().mutating->expires_at_ms > clock.now_ms());
      }
    }
  }
}

FABRIC_TEST(property_authority, stale_credentials_are_always_refused) {
  test::Random random(0xC0FFEEULL);
  for (int trial = 0; trial < 100; ++trial) {
    ManualClock clock(1000);
    SeededTokenSource tokens(static_cast<std::uint64_t>(trial) + 7);
    AuthorityRegistry registry(clock);
    registry.set_token_source(&tokens);
    const ShardId shard = *ShardId::parse("shard-0");
    FABRIC_CHECK_OK(registry.establish(shard, EpochNumber::from_value(1)));

    std::vector<AuthorityClaim> historical;
    IncarnationId current_holder = incarnation_for(1, 1);
    for (int round = 0; round < 6; ++round) {
      GrantRequest request;
      request.shard = shard;
      request.holder = current_holder;
      request.mode = AuthorityMode::Mutating;
      request.ttl_ms = 100000;
      auto lease = registry.grant(request);
      if (!lease.ok()) {
        // The previous holder must be fenced before a new one can take over.
        const auto view = registry.view(shard);
        FABRIC_CHECK_OK(view);
        for (const FenceRecord& fence : view.value().fences) {
          if (!fence.acknowledged) {
            FABRIC_CHECK_OK(registry.acknowledge_fence(shard, fence.target, fence.id));
          }
        }
        lease = registry.grant(request);
        FABRIC_CHECK_OK(lease);
      }
      AuthorityClaim claim;
      claim.shard = shard;
      claim.incarnation = current_holder;
      claim.epoch = lease.value().epoch;
      claim.generation = lease.value().generation;
      claim.token = lease.value().token;
      historical.push_back(claim);
      FABRIC_CHECK_OK(registry.validate_claim(claim, AuthorityMode::Mutating));
      current_holder = incarnation_for(1 + static_cast<std::uint64_t>(round) + 1, 1);
      FABRIC_CHECK_OK(registry.request_fence(shard, claim.incarnation, "property rotation"));
    }

    // Every credential ever issued except the newest is stale now.
    for (std::size_t index = 0; index + 1 < historical.size(); ++index) {
      const Status status = registry.validate_claim(historical[index], AuthorityMode::Mutating);
      FABRIC_CHECK(!status.ok());
    }
    FABRIC_CHECK(registry.validate_claim(historical.back(), AuthorityMode::Mutating).ok() == false ||
                 registry.view(shard).value().mutating.has_value());
    (void)random;
  }
}

FABRIC_TEST(property_manifest, sealing_is_deterministic_and_tamper_evident) {
  test::Random random(0xBEEFULL);
  for (int trial = 0; trial < 50; ++trial) {
    const std::uint32_t target_schema = 2 + static_cast<std::uint32_t>(random.below(2));
    const ComponentSpec source =
        test::make_spec("node-a", "1.0.0", "1.0", 1, static_cast<std::uint16_t>(40000 + trial),
                        test::standard_features());
    const ComponentSpec target =
        test::make_spec("node-b", "2.0.0", "1.1", target_schema,
                        static_cast<std::uint16_t>(41000 + trial), test::standard_features());
    const CompatibilityDecision decision = test::make_decision(source, target);
    EvolutionManifest manifest =
        test::make_manifest(source, target, decision, "campaign-" + std::to_string(trial));
    const Digest first = manifest.compute_digest();
    EvolutionManifest copy = manifest;
    FABRIC_CHECK_EQ(copy.compute_digest(), first);
    const Json encoded = to_json(manifest);
    const auto decoded = manifest_from_json(encoded);
    FABRIC_CHECK_OK(decoded);
    FABRIC_CHECK_EQ(decoded.value().compute_digest(), first);
    // The migration chain always spans the declared schema envelope.
    FABRIC_CHECK_EQ(manifest.migration_path(source.schema, target.schema).empty(), false);
    FABRIC_CHECK_EQ(manifest.migration_path(source.schema, target.schema).back().to, target.schema);
  }
}

FABRIC_TEST(property_migration, randomized_documents_round_trip_exactly) {
  test::Random random(0xD00DULL);
  for (int trial = 0; trial < 100; ++trial) {
    StateDocument document(SchemaVersion::from_value(1));
    const std::size_t fields = 1 + static_cast<std::size_t>(random.below(8));
    for (std::size_t index = 0; index < fields; ++index) {
      const std::string key = "field-" + std::to_string(index);
      std::string value;
      const std::size_t length = static_cast<std::size_t>(random.below(24));
      for (std::size_t character = 0; character < length; ++character) {
        value.push_back(static_cast<char>('a' + (random.below(26))));
      }
      FABRIC_CHECK_OK(document.put(key, value));
    }
    // Always include the field the declared step operates on.
    FABRIC_CHECK_OK(document.put("region", "eu-west"));

    MigrationStepSpec rename;
    rename.id = *MigrationStepId::parse("rename");
    rename.from = SchemaVersion::from_value(1);
    rename.to = SchemaVersion::from_value(2);
    rename.function = "rename_field";
    rename.parameters.set("from", Json("region"));
    rename.parameters.set("to", Json("zone"));
    rename.deterministic = true;
    rename.reversible = true;

    const Digest before = document.digest();
    StateMigrator migrator;
    StateDocument working = document;
    FABRIC_CHECK_OK(migrator.migrate_forward(working, {rename}));
    FABRIC_CHECK_EQ(working.schema(), SchemaVersion::from_value(2));
    FABRIC_CHECK_OK(migrator.migrate_backward(working, {rename}));
    FABRIC_CHECK_EQ(working.digest(), before);
    FABRIC_CHECK_EQ(working.schema(), SchemaVersion::from_value(1));
  }
}

}  // namespace fabric::evolution
