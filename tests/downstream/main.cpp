// Fabric Evolution — independent downstream consumer.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// This program is deliberately NOT part of the Fabric Evolution build tree. It
// is configured with find_package(FabricEvolution CONFIG REQUIRED) against the
// installed artifacts and proves that the exported package is usable: headers
// resolve, the library links, and the runtime behaves.

#include <cstdio>
#include <string>

#include <fabric/evolution/authority.hpp>
#include <fabric/evolution/manifest.hpp>
#include <fabric/evolution/migration.hpp>
#include <fabric/evolution/version.hpp>

namespace {

using namespace fabric::evolution;

int fail(const char* message) {
  std::fprintf(stderr, "downstream consumer failed: %s\n", message);
  return 1;
}

}  // namespace

int main() {
  using namespace fabric::evolution;

  std::printf("Fabric Evolution %s\n", FABRIC_EVOLUTION_VERSION_STRING);

  // 1. Authority semantics are available and enforced.
  ManualClock clock(1000);
  SeededTokenSource tokens(99);
  AuthorityRegistry registry(clock);
  registry.set_token_source(&tokens);
  const ShardId shard = *ShardId::parse("shard-0");
  if (!registry.establish(shard, EpochNumber::from_value(1)).ok()) {
    return fail("cannot establish shard authority");
  }
  GrantRequest request;
  request.shard = shard;
  request.holder = IncarnationId(ComponentId::unchecked("downstream-node"),
                                 IncarnationUuid::parse("00112233445566778899aabbccddeeff").value(),
                                 BootCounter::from_value(1));
  request.mode = AuthorityMode::Mutating;
  request.ttl_ms = 1000;
  auto lease = registry.grant(request);
  if (!lease.ok()) {
    return fail("cannot grant authority");
  }
  AuthorityClaim claim;
  claim.shard = shard;
  claim.incarnation = request.holder;
  claim.epoch = lease.value().epoch;
  claim.generation = lease.value().generation;
  claim.token = lease.value().token;
  if (!registry.validate_claim(claim, AuthorityMode::Mutating).ok()) {
    return fail("a live claim was refused");
  }
  if (!registry.request_fence(shard, request.holder, "downstream fence").ok()) {
    return fail("cannot fence");
  }
  if (registry.validate_claim(claim, AuthorityMode::Mutating).ok()) {
    return fail("a fenced claim was accepted");
  }

  // 2. State migration and the irreversible boundary rule.
  StateDocument document(SchemaVersion::from_value(1));
  (void)document.put("region", "eu-west");
  MigrationStepSpec rename;
  rename.id = *MigrationStepId::parse("rename");
  rename.from = SchemaVersion::from_value(1);
  rename.to = SchemaVersion::from_value(2);
  rename.function = "rename_field";
  rename.parameters.set("from", Json("region"));
  rename.parameters.set("to", Json("zone"));
  StateMigrator migrator;
  auto migrated = migrator.migrate_forward(document, {rename});
  if (!migrated.ok() || document.get("zone").value_or("") != "eu-west") {
    return fail("migration did not produce the expected state");
  }
  MigrationStepSpec drop = rename;
  drop.id = *MigrationStepId::parse("drop");
  drop.from = SchemaVersion::from_value(1);
  drop.to = SchemaVersion::from_value(2);
  drop.function = "remove_field";
  drop.parameters = Json::object({{"key", Json("zone")}});
  drop.reversible = false;
  drop.irreversible_boundary = true;
  StateDocument other(SchemaVersion::from_value(1));
  (void)other.put("zone", "eu-west");
  if (!migrator.migrate_forward(other, {drop}).ok()) {
    return fail("cannot cross the declared boundary");
  }
  if (migrator.migrate_backward(other, {drop}).ok()) {
    return fail("a downgrade across an irreversible boundary was accepted");
  }

  // 3. JSON and digests are usable from downstream code.
  const Digest digest = sha256("downstream");
  if (digest.to_hex().size() != 64) {
    return fail("digest rendering is wrong");
  }
  Json document_json = Json::object();
  document_json.set("status", Json("ok"));
  if (document_json.dump() != "{\"status\":\"ok\"}") {
    return fail("canonical JSON encoding is wrong");
  }

  std::printf("downstream consumer verified the installed Fabric Evolution package\n");
  return 0;
}
