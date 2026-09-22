// Fabric Evolution — shipped example documents must be valid and consistent.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The example compatibility registry and evolution manifest are documentation, so
// they are checked the same way real input is: the manifest must be admissible and
// its embedded evidence must match the registry document.

#include <fstream>
#include <sstream>
#include <string>

#include "fabric/evolution/compatibility.hpp"
#include "fabric/evolution/manifest.hpp"
#include "support/test_harness.hpp"

#ifndef FABRIC_EVOLUTION_SOURCE_DIR
#error "FABRIC_EVOLUTION_SOURCE_DIR must be defined by the build system"
#endif

namespace fabric::evolution {
namespace {

[[nodiscard]] std::string read_text(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    FABRIC_FAIL("cannot read " + path);
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

[[nodiscard]] std::string example(const std::string& name) {
  return std::string(FABRIC_EVOLUTION_SOURCE_DIR) + "/examples/manifests/" + name;
}

}  // namespace

FABRIC_TEST(examples, shipped_manifest_is_admissible_and_consistent_with_the_registry) {
  auto manifest = manifest_from_json_text(read_text(example("evolution-manifest.json")));
  FABRIC_CHECK_OK(manifest);
  FABRIC_CHECK_OK(manifest.value().validate());
  FABRIC_CHECK_OK(manifest.value().verify_digest());
  FABRIC_CHECK_EQ(manifest.value().shard, *ShardId::parse("shard-0"));
  FABRIC_CHECK_EQ(manifest.value().source.schema, SchemaVersion::from_value(1));
  FABRIC_CHECK_EQ(manifest.value().target.schema, SchemaVersion::from_value(2));
  FABRIC_CHECK_EQ(manifest.value().migration_path(SchemaVersion::from_value(1),
                                                  SchemaVersion::from_value(2))
                      .size(),
                  static_cast<std::size_t>(1));

  auto registry = JsonCompatibilityRegistry::parse(read_text(example("compatibility-registry.json")));
  FABRIC_CHECK_OK(registry);
  FABRIC_CHECK_EQ(registry.value().size(), static_cast<std::size_t>(1));
  const EvidenceCheck evidence =
      verify_evidence(manifest.value().compatibility, registry.value(), EvidencePolicy::RequireExactMatch);
  FABRIC_CHECK(evidence.satisfied);
  FABRIC_CHECK(!evidence.refreshed);
  FABRIC_CHECK_EQ(evidence.rationale, std::string("registry evidence matches the manifest digest exactly"));

  // The document-level decisions really do cover the endpoints the manifest names.
  auto decision = registry.value().decide(manifest.value().source.id, manifest.value().source.software,
                                          manifest.value().target.software);
  FABRIC_CHECK_OK(decision);
  FABRIC_CHECK_EQ(decision.value().verdict, CompatibilityVerdict::SupportedWithConstraints);
  FABRIC_CHECK(decision.value().certified_features.contains_all(
      manifest.value().protocol.required_features));
}

FABRIC_TEST(examples, shipped_registry_rejects_pairs_it_does_not_cover) {
  auto registry = JsonCompatibilityRegistry::parse(read_text(example("compatibility-registry.json")));
  FABRIC_CHECK_OK(registry);
  const auto component = ComponentId::parse("node-a");
  FABRIC_CHECK(component.has_value());
  const auto source = SoftwareVersion::parse("1.0.0");
  const auto target = SoftwareVersion::parse("9.9.9");
  FABRIC_CHECK(source.has_value() && target.has_value());
  FABRIC_CHECK_ERR(registry.value().decide(*component, *source, *target),
                   ErrorCode::CompatibilityInsufficient);
}

}  // namespace fabric::evolution
