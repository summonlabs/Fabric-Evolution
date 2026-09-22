// Fabric Evolution — consumed compatibility decisions.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Fabric Evolution does not compute compatibility itself. It *consumes* typed
// compatibility decisions produced by the Fabric Compatibility Registry, binds
// them to a manifest by digest, and refuses to evolve when the evidence is
// missing, unsupported, or no longer matches what the registry now says.

#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "fabric/evolution/digest.hpp"
#include "fabric/evolution/features.hpp"
#include "fabric/evolution/ids.hpp"
#include "fabric/evolution/versions.hpp"

namespace fabric::evolution {

enum class CompatibilityVerdict : std::uint8_t {
  Unsupported = 0,
  SupportedWithConstraints = 1,
  Supported = 2,
};

[[nodiscard]] std::string_view to_string(CompatibilityVerdict verdict) noexcept;
[[nodiscard]] std::optional<CompatibilityVerdict> compatibility_verdict_from(std::string_view text);

struct CompatibilityConstraint {
  std::string name;
  std::string value;
};

struct CompatibilityDecision {
  std::string decision_id;
  std::string registry;
  ComponentId subject;
  SoftwareVersion source;
  SoftwareVersion target;
  ProtocolVersion source_protocol;
  ProtocolVersion target_protocol;
  SchemaVersion source_schema;
  SchemaVersion target_schema;
  CompatibilityVerdict verdict = CompatibilityVerdict::Unsupported;
  bool rollback_permitted = false;
  std::uint64_t max_mixed_version_operations = 0;
  std::uint64_t max_mixed_version_ms = 0;
  // Features the registry certifies as mutually supported across the mixed
  // version window. A feature absent here can never be enabled, even if both
  // participants advertise it.
  FeatureSet certified_features;
  std::vector<CompatibilityConstraint> constraints;
  EpochNumber decided_at;
  Digest evidence_digest;

  // Digest is over the canonical encoding of every field except evidence_digest.
  [[nodiscard]] Digest compute_digest() const;
  [[nodiscard]] Status seal();
  [[nodiscard]] Status verify_digest() const;
};

[[nodiscard]] Json to_json(const CompatibilityDecision& decision);
[[nodiscard]] Result<CompatibilityDecision> compatibility_decision_from_json(const Json& value);

// The registry interface. Implementations are supplied by the deployment: this
// repository ships a static in-memory registry and a JSON-file registry, and
// both are exercised by the test suite.
class CompatibilityRegistry {
 public:
  CompatibilityRegistry() = default;
  virtual ~CompatibilityRegistry();
  CompatibilityRegistry(const CompatibilityRegistry&) = delete;
  CompatibilityRegistry& operator=(const CompatibilityRegistry&) = delete;

  [[nodiscard]] virtual Result<CompatibilityDecision> decide(const ComponentId& subject,
                                                             const SoftwareVersion& source,
                                                             const SoftwareVersion& target) const = 0;
  [[nodiscard]] virtual std::string name() const = 0;

 protected:
  CompatibilityRegistry(CompatibilityRegistry&&) noexcept = default;
  CompatibilityRegistry& operator=(CompatibilityRegistry&&) noexcept = default;
};

class StaticCompatibilityRegistry final : public CompatibilityRegistry {
 public:
  void add(CompatibilityDecision decision);
  [[nodiscard]] Result<CompatibilityDecision> decide(const ComponentId& subject,
                                                     const SoftwareVersion& source,
                                                     const SoftwareVersion& target) const override;
  [[nodiscard]] std::string name() const override { return "static"; }
  [[nodiscard]] std::size_t size() const noexcept { return decisions_.size(); }

 private:
  std::vector<CompatibilityDecision> decisions_;
};

// JSON-backed registry. The document shape is:
//   { "registry": "<name>", "decisions": [ <decision>, ... ] }
class JsonCompatibilityRegistry final : public CompatibilityRegistry {
 public:
  JsonCompatibilityRegistry() = default;
  JsonCompatibilityRegistry(JsonCompatibilityRegistry&&) noexcept = default;
  JsonCompatibilityRegistry& operator=(JsonCompatibilityRegistry&&) noexcept = default;

  [[nodiscard]] static Result<JsonCompatibilityRegistry> parse(std::string_view text);
  [[nodiscard]] static Result<JsonCompatibilityRegistry> load_file(const std::string& path);

  [[nodiscard]] Result<CompatibilityDecision> decide(const ComponentId& subject,
                                                     const SoftwareVersion& source,
                                                     const SoftwareVersion& target) const override;
  [[nodiscard]] std::string name() const override { return registry_; }
  [[nodiscard]] std::size_t size() const noexcept { return decisions_.size(); }

 private:
  std::string registry_ = "fabric-compatibility-registry";
  std::vector<CompatibilityDecision> decisions_;
};

// How strictly a manifest's embedded evidence must match the registry's current
// decision.
enum class EvidencePolicy : std::uint8_t {
  RequireExactMatch = 0,             // the registry decision digest must be identical
  AllowRefreshIfVerdictUnchanged = 1 // a refreshed decision is accepted when the verdict is the same
};

struct EvidenceCheck {
  bool satisfied = false;
  bool refreshed = false;
  CompatibilityDecision effective;
  std::string rationale;
  std::vector<std::string> differences;
};

[[nodiscard]] EvidenceCheck verify_evidence(const CompatibilityDecision& declared,
                                            const CompatibilityRegistry& registry,
                                            EvidencePolicy policy);

}  // namespace fabric::evolution
