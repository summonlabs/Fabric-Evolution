// Fabric Evolution — mixed-version protocol negotiation (implementation).
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric/evolution/protocol.hpp"

#include <algorithm>

namespace fabric::evolution {
namespace {

[[nodiscard]] Json feature_array(const FeatureSet& features) {
  Json out = Json::array();
  for (const std::string& name : features.names()) {
    out.push_back(Json(name));
  }
  return out;
}

[[nodiscard]] bool schema_allowed(const std::vector<SchemaVersion>& allowed, SchemaVersion schema) {
  return std::find(allowed.begin(), allowed.end(), schema) != allowed.end();
}

}  // namespace

bool ProtocolHello::is_valid() const noexcept {
  return component.is_valid() && incarnation.is_valid() && software.is_valid() && protocol.is_valid() &&
         schema.is_valid();
}

Json to_json(const ProtocolHello& hello) {
  Json out = Json::object();
  out.set("component", Json(hello.component.str()));
  out.set("incarnation", to_json(hello.incarnation));
  out.set("software", Json(hello.software.to_string()));
  out.set("protocol", Json(hello.protocol.to_string()));
  out.set("schema", Json(hello.schema.value()));
  out.set("features", feature_array(hello.features));
  out.set("epoch", Json(hello.epoch.value()));
  out.set("generation", Json(hello.generation.value()));
  out.set("holds_authority", Json(hello.holds_authority));
  return out;
}

Result<ProtocolHello> protocol_hello_from_json(const Json& value) {
  if (!value.is_object()) {
    return malformed("protocol hello must be an object");
  }
  ProtocolHello hello;
  const auto component = json_string(value, "component");
  if (!component.has_value()) {
    return malformed("protocol hello is missing component");
  }
  auto parsed_component = ComponentId::parse(*component);
  if (!parsed_component.has_value()) {
    return malformed("protocol hello component is invalid: " + *component);
  }
  hello.component = *parsed_component;

  const Json* incarnation = value.find("incarnation");
  if (incarnation == nullptr) {
    return malformed("protocol hello is missing incarnation");
  }
  auto parsed_incarnation = incarnation_from_json(*incarnation);
  if (!parsed_incarnation.ok()) {
    return parsed_incarnation.status();
  }
  hello.incarnation = parsed_incarnation.value();

  const auto software = json_string(value, "software");
  if (!software.has_value()) {
    return malformed("protocol hello is missing software version");
  }
  auto parsed_software = SoftwareVersion::parse(*software);
  if (!parsed_software.has_value()) {
    return malformed("protocol hello software version is invalid: " + *software);
  }
  hello.software = *parsed_software;

  const auto protocol = json_string(value, "protocol");
  if (!protocol.has_value()) {
    return malformed("protocol hello is missing protocol version");
  }
  auto parsed_protocol = ProtocolVersion::parse(*protocol);
  if (!parsed_protocol.has_value()) {
    return malformed("protocol hello protocol version is invalid: " + *protocol);
  }
  hello.protocol = *parsed_protocol;

  const auto schema = json_u64(value, "schema");
  if (!schema.has_value() || *schema == 0 || *schema > UINT32_MAX) {
    return malformed("protocol hello schema version is missing or out of range");
  }
  hello.schema = SchemaVersion::from_value(static_cast<std::uint32_t>(*schema));

  if (const Json* features = value.find("features")) {
    if (!features->is_array()) {
      return malformed("protocol hello features must be an array");
    }
    std::vector<std::string> names;
    for (std::size_t index = 0; index < features->size(); ++index) {
      const std::string* name = features->at(index).try_string();
      if (name == nullptr) {
        return malformed("protocol hello feature entries must be strings");
      }
      names.push_back(*name);
    }
    std::string error;
    const auto parsed = FeatureSet::from_names(names, error);
    if (!parsed.has_value()) {
      return malformed(error);
    }
    hello.features = *parsed;
  }

  hello.epoch = EpochNumber::from_value(json_u64_or(value, "epoch", 0));
  hello.generation = Generation::from_value(json_u64_or(value, "generation", 0));
  hello.holds_authority = json_bool_or(value, "holds_authority", false);
  return hello;
}

std::string_view to_string(NegotiationMode mode) noexcept {
  switch (mode) {
    case NegotiationMode::Refused:
      return "refused";
    case NegotiationMode::Restricted:
      return "restricted";
    case NegotiationMode::Compatible:
      return "compatible";
  }
  return "unknown";
}

Json NegotiatedProtocol::to_json() const {
  Json out = Json::object();
  out.set("mode", Json(std::string(::fabric::evolution::to_string(mode))));
  out.set("effective", Json(effective.to_string()));
  out.set("enabled_features", feature_array(enabled));
  out.set("disabled_features", feature_array(disabled));
  Json refusal_array = Json::array();
  for (const std::string& refusal : refusals) {
    refusal_array.push_back(Json(refusal));
  }
  out.set("refusals", std::move(refusal_array));
  out.set("rationale", Json(rationale));
  return out;
}

NegotiatedProtocol negotiate(const ProtocolHello& local, const ProtocolHello& peer,
                             const ProtocolPath& path,
                             const std::vector<SchemaVersion>& allowed_schemas) {
  NegotiatedProtocol result;
  if (!local.is_valid() || !peer.is_valid()) {
    result.refusals.push_back("participant hello is incomplete");
    result.rationale = "refused: a participant did not present a complete hello";
    return result;
  }
  if (!local.protocol.compatible_with(peer.protocol)) {
    result.refusals.push_back("protocol major mismatch: " + local.protocol.to_string() + " vs " +
                              peer.protocol.to_string());
    result.rationale = "refused: protocol major versions differ";
    return result;
  }
  const ProtocolVersion effective = local.protocol.negotiate(peer.protocol);
  if (!path.accepts(effective)) {
    result.refusals.push_back("effective protocol version " + effective.to_string() +
                              " is not on the manifest negotiation path");
    result.rationale = "refused: negotiated protocol version is not declared";
    return result;
  }
  if (!schema_allowed(allowed_schemas, local.schema)) {
    result.refusals.push_back(local.component.str() + " reports schema " +
                              std::to_string(local.schema.value()) + " which the manifest does not list");
  }
  if (!schema_allowed(allowed_schemas, peer.schema)) {
    result.refusals.push_back(peer.component.str() + " reports schema " +
                              std::to_string(peer.schema.value()) + " which the manifest does not list");
  }
  if (!result.refusals.empty()) {
    result.rationale = "refused: a participant reports a schema outside the declared migration envelope";
    return result;
  }

  const FeatureSet common = local.features.intersect(peer.features).intersect(path.allowed_features);
  const FeatureSet missing_required = path.required_features.without(common);
  if (!missing_required.empty()) {
    result.missing_required = missing_required;
    for (const std::string& name : missing_required.names()) {
      result.refusals.push_back("required feature not supported by every participant: " + name);
    }
    result.rationale = "refused: the manifest requires features the participants do not jointly support";
    return result;
  }

  result.effective = effective;
  result.enabled = common;
  result.disabled = path.allowed_features.without(common);
  result.mode = (effective == local.protocol && effective == peer.protocol)
                    ? NegotiationMode::Compatible
                    : NegotiationMode::Restricted;
  result.rationale = result.mode == NegotiationMode::Compatible
                         ? "compatible: participants agree on protocol and features"
                         : "restricted: operating at the lower common protocol version " +
                               effective.to_string();
  return result;
}

NegotiatedProtocol negotiate_group(const GroupNegotiationRequest& request) {
  NegotiatedProtocol result;
  if (request.participants.empty()) {
    result.refusals.push_back("no participants presented a hello");
    result.rationale = "refused: no participants";
    return result;
  }
  for (const ComponentId& required : request.required_participants) {
    bool present = false;
    for (const ProtocolHello& hello : request.participants) {
      if (hello.component == required) {
        present = true;
        break;
      }
    }
    if (!present) {
      result.refusals.push_back("required participant did not present a hello: " + required.str());
    }
  }
  if (!result.refusals.empty()) {
    result.rationale = "refused: a required participant is absent";
    return result;
  }

  ProtocolVersion effective = request.participants.front().protocol;
  FeatureSet common = request.participants.front().features;
  for (const ProtocolHello& hello : request.participants) {
    if (!hello.is_valid()) {
      result.refusals.push_back("participant hello is incomplete: " + hello.component.str());
      result.rationale = "refused: a participant did not present a complete hello";
      return result;
    }
    if (!effective.compatible_with(hello.protocol)) {
      result.refusals.push_back("protocol major mismatch: " + effective.to_string() + " vs " +
                                hello.protocol.to_string());
      result.rationale = "refused: protocol major versions differ across participants";
      return result;
    }
    effective = effective.negotiate(hello.protocol);
    common = common.intersect(hello.features);
    if (!schema_allowed(request.allowed_schemas, hello.schema)) {
      result.refusals.push_back(hello.component.str() + " reports schema " +
                                std::to_string(hello.schema.value()) +
                                " which the manifest does not list");
    }
  }
  if (!result.refusals.empty()) {
    result.rationale = "refused: a participant reports a schema outside the declared migration envelope";
    return result;
  }
  if (!request.path.accepts(effective)) {
    result.refusals.push_back("effective protocol version " + effective.to_string() +
                              " is not on the manifest negotiation path");
    result.rationale = "refused: negotiated protocol version is not declared";
    return result;
  }

  const FeatureSet enabled = common.intersect(request.path.allowed_features);
  const FeatureSet missing_required = request.path.required_features.without(enabled);
  if (!missing_required.empty()) {
    result.missing_required = missing_required;
    for (const std::string& name : missing_required.names()) {
      result.refusals.push_back("required feature not supported by every participant: " + name);
    }
    result.rationale = "refused: the manifest requires features the participants do not jointly support";
    return result;
  }

  result.effective = effective;
  result.enabled = enabled;
  result.disabled = request.path.allowed_features.without(enabled);
  bool every_participant_at_effective = true;
  for (const ProtocolHello& hello : request.participants) {
    if (hello.protocol != effective) {
      every_participant_at_effective = false;
      break;
    }
  }
  result.mode = every_participant_at_effective ? NegotiationMode::Compatible : NegotiationMode::Restricted;
  result.rationale = result.mode == NegotiationMode::Compatible
                         ? "compatible: all participants agree on protocol and features"
                         : "restricted: operating at the lower common protocol version " +
                               effective.to_string();
  return result;
}

}  // namespace fabric::evolution
