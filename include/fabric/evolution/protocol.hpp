// Fabric Evolution — mixed-version protocol negotiation and feature gating.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Negotiation is deterministic and total: given the same participant hellos and
// the same manifest protocol path, every participant computes the same effective
// protocol version and the same enabled feature set. A feature is enabled only
// when every required participant advertises it, the manifest allows it, and the
// compatibility registry has certified it (the manifest already guarantees the
// last two by construction).

#pragma once

#include <string>
#include <vector>

#include "fabric/evolution/features.hpp"
#include "fabric/evolution/ids.hpp"
#include "fabric/evolution/manifest.hpp"
#include "fabric/evolution/versions.hpp"

namespace fabric::evolution {

struct ProtocolHello {
  ComponentId component;
  IncarnationId incarnation;
  SoftwareVersion software;
  ProtocolVersion protocol;
  SchemaVersion schema;
  FeatureSet features;
  EpochNumber epoch;
  Generation generation;
  bool holds_authority = false;

  [[nodiscard]] bool is_valid() const noexcept;
};

[[nodiscard]] Json to_json(const ProtocolHello& hello);
[[nodiscard]] Result<ProtocolHello> protocol_hello_from_json(const Json& value);

enum class NegotiationMode : std::uint8_t {
  Refused = 0,     // the participants may not interoperate at all
  Restricted = 1,  // interoperable at a reduced protocol version or feature set
  Compatible = 2,  // interoperable at both participants' own versions
};

[[nodiscard]] std::string_view to_string(NegotiationMode mode) noexcept;

struct NegotiatedProtocol {
  NegotiationMode mode = NegotiationMode::Refused;
  ProtocolVersion effective;
  FeatureSet enabled;
  FeatureSet disabled;
  // Required features that at least one participant did not offer. Keeping this
  // separate lets the caller refuse with the precise reason.
  FeatureSet missing_required;
  std::vector<std::string> refusals;
  std::string rationale;

  [[nodiscard]] bool usable() const noexcept { return mode != NegotiationMode::Refused; }
  [[nodiscard]] bool supports(Feature feature) const noexcept { return enabled.contains(feature); }
  [[nodiscard]] Json to_json() const;
};

// Two-party negotiation, used when a controller talks to one component.
[[nodiscard]] NegotiatedProtocol negotiate(const ProtocolHello& local, const ProtocolHello& peer,
                                           const ProtocolPath& path,
                                           const std::vector<SchemaVersion>& allowed_schemas);

// Multi-party negotiation: the result is usable only when every participant
// agrees. Every component listed in required_participants must present a hello,
// and the enabled feature set is the intersection over all participants.
struct GroupNegotiationRequest {
  std::vector<ProtocolHello> participants;
  std::vector<ComponentId> required_participants;
  ProtocolPath path;
  std::vector<SchemaVersion> allowed_schemas;
};

[[nodiscard]] NegotiatedProtocol negotiate_group(const GroupNegotiationRequest& request);

}  // namespace fabric::evolution
