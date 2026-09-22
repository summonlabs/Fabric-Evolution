// Fabric Evolution — protocol negotiation and feature gating tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <string>
#include <vector>

#include "fabric/evolution/protocol.hpp"
#include "support/cluster.hpp"
#include "support/test_harness.hpp"

namespace fabric::evolution {
namespace {

ProtocolHello hello_of(const char* component, const char* software, const char* protocol,
                       std::uint32_t schema, FeatureSet features) {
  ProtocolHello hello;
  hello.component = *ComponentId::parse(component);
  hello.incarnation =
      IncarnationId(hello.component, IncarnationUuid::parse("00112233445566778899aabbccddeeff").value(),
                    BootCounter::from_value(1));
  hello.software = *SoftwareVersion::parse(software);
  hello.protocol = *ProtocolVersion::parse(protocol);
  hello.schema = SchemaVersion::from_value(schema);
  hello.features = features;
  hello.epoch = EpochNumber::from_value(1);
  hello.generation = Generation::from_value(1);
  return hello;
}

ProtocolPath path_with(FeatureSet required, FeatureSet allowed) {
  ProtocolPath path;
  path.accepted = {ProtocolVersion::of(1, 0), ProtocolVersion::of(1, 1)};
  path.required_features = required;
  path.allowed_features = allowed;
  return path;
}

}  // namespace

FABRIC_TEST(protocol, compatible_participants_agree) {
  const FeatureSet features = test::standard_features();
  const ProtocolHello a = hello_of("node-a", "1.0.0", "1.1", 1, features);
  const ProtocolHello b = hello_of("node-b", "2.0.0", "1.1", 2, features);
  const NegotiatedProtocol negotiated =
      negotiate(a, b, path_with(FeatureSet::of(Feature::SnapshotTransfer), features),
                {SchemaVersion::from_value(1), SchemaVersion::from_value(2)});
  FABRIC_CHECK(negotiated.usable());
  FABRIC_CHECK_EQ(negotiated.mode, NegotiationMode::Compatible);
  FABRIC_CHECK_EQ(negotiated.effective, ProtocolVersion::of(1, 1));
  FABRIC_CHECK(negotiated.supports(Feature::SnapshotTransfer));
}

FABRIC_TEST(protocol, lower_minor_wins_and_restricts) {
  const FeatureSet features = test::standard_features();
  const ProtocolHello a = hello_of("node-a", "1.0.0", "1.0", 1, features);
  const ProtocolHello b = hello_of("node-b", "2.0.0", "1.1", 2, features);
  const NegotiatedProtocol negotiated =
      negotiate(a, b, path_with(FeatureSet::none(), features),
                {SchemaVersion::from_value(1), SchemaVersion::from_value(2)});
  FABRIC_CHECK(negotiated.usable());
  FABRIC_CHECK_EQ(negotiated.mode, NegotiationMode::Restricted);
  FABRIC_CHECK_EQ(negotiated.effective, ProtocolVersion::of(1, 0));
}

FABRIC_TEST(protocol, major_mismatch_is_refused) {
  const FeatureSet features = test::standard_features();
  const ProtocolHello a = hello_of("node-a", "1.0.0", "1.0", 1, features);
  const ProtocolHello b = hello_of("node-b", "2.0.0", "2.0", 2, features);
  ProtocolPath path = path_with(FeatureSet::none(), features);
  path.accepted = {ProtocolVersion::of(1, 0), ProtocolVersion::of(2, 0)};
  const NegotiatedProtocol negotiated =
      negotiate(a, b, path, {SchemaVersion::from_value(1), SchemaVersion::from_value(2)});
  FABRIC_CHECK(!negotiated.usable());
  FABRIC_CHECK_EQ(negotiated.mode, NegotiationMode::Refused);
  FABRIC_CHECK_EQ(negotiated.refusals.size(), static_cast<std::size_t>(1));
}

FABRIC_TEST(protocol, feature_supported_by_only_one_participant_is_refused) {
  FeatureSet restricted = test::standard_features();
  restricted = restricted.without(FeatureSet::of(Feature::IncrementalCatchUp));
  const ProtocolHello a = hello_of("node-a", "1.0.0", "1.1", 1, test::standard_features());
  const ProtocolHello b = hello_of("node-b", "2.0.0", "1.1", 2, restricted);
  ProtocolPath path = path_with(FeatureSet::of(Feature::IncrementalCatchUp), test::standard_features());
  const NegotiatedProtocol negotiated =
      negotiate(a, b, path, {SchemaVersion::from_value(1), SchemaVersion::from_value(2)});
  FABRIC_CHECK(!negotiated.usable());
  FABRIC_CHECK(negotiated.refusals.front().find("incremental_catch_up") != std::string::npos);
}

FABRIC_TEST(protocol, feature_absent_from_the_manifest_path_is_never_enabled) {
  const FeatureSet features = test::standard_features();
  FeatureSet offered = features;
  offered = offered.without(FeatureSet::of(Feature::ChecksummedRecords));
  const ProtocolHello a = hello_of("node-a", "1.0.0", "1.1", 1, features);
  const ProtocolHello b = hello_of("node-b", "2.0.0", "1.1", 2, offered);
  FeatureSet allowed = features;
  allowed = allowed.without(FeatureSet::of(Feature::ForwardRecovery));
  const NegotiatedProtocol negotiated =
      negotiate(a, b, path_with(FeatureSet::none(), allowed),
                {SchemaVersion::from_value(1), SchemaVersion::from_value(2)});
  FABRIC_CHECK(negotiated.usable());
  FABRIC_CHECK(!negotiated.supports(Feature::ForwardRecovery));
  // A feature outside the manifest path is simply not negotiable; a feature the
  // path allows but a participant lacks is reported as disabled.
  FABRIC_CHECK(!allowed.contains(Feature::ForwardRecovery));
  FABRIC_CHECK(negotiated.disabled.contains(Feature::ChecksummedRecords));
}

FABRIC_TEST(protocol, schema_outside_the_declared_envelope_is_refused) {
  const FeatureSet features = test::standard_features();
  const ProtocolHello a = hello_of("node-a", "1.0.0", "1.1", 7, features);
  const ProtocolHello b = hello_of("node-b", "2.0.0", "1.1", 2, features);
  const NegotiatedProtocol negotiated =
      negotiate(a, b, path_with(FeatureSet::none(), features),
                {SchemaVersion::from_value(1), SchemaVersion::from_value(2)});
  FABRIC_CHECK(!negotiated.usable());
}

FABRIC_TEST(protocol, group_negotiation_requires_every_participant) {
  GroupNegotiationRequest request;
  request.participants.push_back(hello_of("node-a", "1.0.0", "1.1", 1, test::standard_features()));
  request.participants.push_back(hello_of("node-b", "2.0.0", "1.1", 2, test::standard_features()));
  request.required_participants.push_back(*ComponentId::parse("node-a"));
  request.required_participants.push_back(*ComponentId::parse("node-b"));
  request.path = path_with(FeatureSet::of(Feature::SnapshotTransfer), test::standard_features());
  request.allowed_schemas = {SchemaVersion::from_value(1), SchemaVersion::from_value(2)};
  FABRIC_CHECK(negotiate_group(request).usable());

  request.required_participants.push_back(*ComponentId::parse("node-c"));
  const NegotiatedProtocol refused = negotiate_group(request);
  FABRIC_CHECK(!refused.usable());
  FABRIC_CHECK(refused.refusals.front().find("node-c") != std::string::npos);
}

FABRIC_TEST(protocol, group_negotiation_intersects_features) {
  FeatureSet limited = test::standard_features();
  limited = limited.without(FeatureSet::of(Feature::PipelinedCatchUp));
  GroupNegotiationRequest request;
  request.participants.push_back(hello_of("node-a", "1.0.0", "1.1", 1, test::standard_features()));
  request.participants.push_back(hello_of("node-b", "2.0.0", "1.1", 2, limited));
  request.required_participants.push_back(*ComponentId::parse("node-a"));
  request.path = path_with(FeatureSet::none(), test::standard_features());
  request.allowed_schemas = {SchemaVersion::from_value(1), SchemaVersion::from_value(2)};
  const NegotiatedProtocol negotiated = negotiate_group(request);
  FABRIC_CHECK(negotiated.usable());
  FABRIC_CHECK(!negotiated.supports(Feature::PipelinedCatchUp));
}

FABRIC_TEST(protocol, negotiation_is_deterministic) {
  const FeatureSet features = test::standard_features();
  const ProtocolHello a = hello_of("node-a", "1.0.0", "1.1", 1, features);
  const ProtocolHello b = hello_of("node-b", "2.0.0", "1.2", 2, features);
  const ProtocolPath path = path_with(FeatureSet::of(Feature::SnapshotTransfer), features);
  const NegotiatedProtocol first =
      negotiate(a, b, path, {SchemaVersion::from_value(1), SchemaVersion::from_value(2)});
  const NegotiatedProtocol second =
      negotiate(a, b, path, {SchemaVersion::from_value(1), SchemaVersion::from_value(2)});
  FABRIC_CHECK_EQ(first.to_json().dump(), second.to_json().dump());
  const NegotiatedProtocol reversed =
      negotiate(b, a, path, {SchemaVersion::from_value(1), SchemaVersion::from_value(2)});
  FABRIC_CHECK_EQ(reversed.to_json().dump(), first.to_json().dump());
}

FABRIC_TEST(protocol, hello_json_round_trip_and_validation) {
  const ProtocolHello hello = hello_of("node-a", "1.0.0", "1.1", 1, test::standard_features());
  const Json encoded = to_json(hello);
  const auto decoded = protocol_hello_from_json(encoded);
  FABRIC_CHECK_OK(decoded);
  FABRIC_CHECK_EQ(decoded.value().component, hello.component);
  FABRIC_CHECK_EQ(decoded.value().features.bits(), hello.features.bits());
  FABRIC_CHECK_ERR(protocol_hello_from_json(Json::object()), ErrorCode::Malformed);
  Json bad_features = encoded;
  bad_features.set("features", Json::array({Json("no_such_feature")}));
  FABRIC_CHECK_ERR(protocol_hello_from_json(bad_features), ErrorCode::Malformed);
  Json bad_protocol = encoded;
  bad_protocol.set("protocol", Json("1"));
  FABRIC_CHECK_ERR(protocol_hello_from_json(bad_protocol), ErrorCode::Malformed);
}

}  // namespace fabric::evolution
