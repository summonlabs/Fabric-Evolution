// Fabric Evolution — domain identities (implementation).
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric/evolution/ids.hpp"

#include <vector>

namespace fabric::evolution {
namespace {

[[nodiscard]] std::optional<std::uint64_t> parse_u64(std::string_view text) {
  if (text.empty() || text.size() > 20) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  for (char c : text) {
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    if (value > (UINT64_MAX - digit) / 10ULL) {
      return std::nullopt;
    }
    value = value * 10ULL + digit;
  }
  return value;
}

[[nodiscard]] std::vector<std::string_view> split(std::string_view text, char separator) {
  std::vector<std::string_view> parts;
  std::size_t start = 0;
  while (true) {
    const std::size_t position = text.find(separator, start);
    if (position == std::string_view::npos) {
      parts.push_back(text.substr(start));
      return parts;
    }
    parts.push_back(text.substr(start, position - start));
    start = position + 1;
  }
}

template <class Tag, class Policy>
[[nodiscard]] Result<TaggedName<Tag, Policy>> name_from_json(const Json& value, const char* what) {
  const std::string* text = value.try_string();
  if (text == nullptr) {
    return malformed(std::string(what) + " must be a string");
  }
  auto parsed = TaggedName<Tag, Policy>::parse(*text);
  if (!parsed.has_value()) {
    return malformed(std::string("invalid ") + what + ": " + *text);
  }
  return *parsed;
}

}  // namespace

std::string IncarnationId::to_string() const {
  return component_.str() + "/" + std::to_string(boot_.value()) + "/" + uuid_.to_compact_string();
}

std::optional<IncarnationId> IncarnationId::parse(std::string_view text) {
  const std::vector<std::string_view> parts = split(text, '/');
  if (parts.size() != 3) {
    return std::nullopt;
  }
  auto component = ComponentId::parse(parts[0]);
  if (!component.has_value()) {
    return std::nullopt;
  }
  const auto boot = parse_u64(parts[1]);
  if (!boot.has_value() || *boot == 0) {
    return std::nullopt;
  }
  auto uuid = IncarnationUuid::parse(parts[2]);
  if (!uuid.has_value() || uuid->is_nil()) {
    return std::nullopt;
  }
  return IncarnationId(*component, *uuid, BootCounter::from_value(*boot));
}

Json to_json(const IncarnationId& incarnation) {
  Json out = Json::object();
  out.set("component", Json(incarnation.component().str()));
  out.set("boot", Json(incarnation.boot().value()));
  out.set("uuid", Json(incarnation.uuid().to_compact_string()));
  return out;
}

Result<IncarnationId> incarnation_from_json(const Json& value) {
  if (!value.is_object()) {
    return malformed("incarnation must be an object");
  }
  const Json* component_value = value.find("component");
  if (component_value == nullptr) {
    return malformed("incarnation is missing component");
  }
  auto component = name_from_json<ComponentTag, NamePolicy<ComponentTag>>(*component_value, "component");
  if (!component.ok()) {
    return component.status();
  }
  const auto boot = json_u64(value, "boot");
  if (!boot.has_value() || *boot == 0) {
    return malformed("incarnation.boot must be a positive integer");
  }
  const auto uuid_text = json_string(value, "uuid");
  if (!uuid_text.has_value()) {
    return malformed("incarnation.uuid must be a string");
  }
  auto uuid = IncarnationUuid::parse(*uuid_text);
  if (!uuid.has_value() || uuid->is_nil()) {
    return malformed("incarnation.uuid is not a valid uuid");
  }
  return IncarnationId(component.value(), *uuid, BootCounter::from_value(*boot));
}

Json to_json(const AuthorityClaim& claim) {
  Json out = Json::object();
  out.set("shard", Json(claim.shard.str()));
  out.set("incarnation", to_json(claim.incarnation));
  out.set("epoch", Json(claim.epoch.value()));
  out.set("generation", Json(claim.generation.value()));
  out.set("token", Json(claim.token.to_compact_string()));
  return out;
}

Result<AuthorityClaim> authority_claim_from_json(const Json& value) {
  if (!value.is_object()) {
    return malformed("authority claim must be an object");
  }
  AuthorityClaim claim;
  const Json* shard_value = value.find("shard");
  if (shard_value == nullptr) {
    return malformed("authority claim is missing shard");
  }
  auto shard = name_from_json<ShardTag, NamePolicy<ShardTag>>(*shard_value, "shard");
  if (!shard.ok()) {
    return shard.status();
  }
  claim.shard = shard.value();

  const Json* incarnation_value = value.find("incarnation");
  if (incarnation_value == nullptr) {
    return malformed("authority claim is missing incarnation");
  }
  auto incarnation = incarnation_from_json(*incarnation_value);
  if (!incarnation.ok()) {
    return incarnation.status();
  }
  claim.incarnation = incarnation.value();

  const auto epoch = json_u64(value, "epoch");
  if (!epoch.has_value() || *epoch == 0) {
    return malformed("authority claim epoch must be a positive integer");
  }
  claim.epoch = EpochNumber::from_value(*epoch);

  const auto generation = json_u64(value, "generation");
  if (!generation.has_value() || *generation == 0) {
    return malformed("authority claim generation must be a positive integer");
  }
  claim.generation = Generation::from_value(*generation);

  const auto token_text = json_string(value, "token");
  if (!token_text.has_value()) {
    return malformed("authority claim is missing token");
  }
  auto token = AuthorityToken::parse(*token_text);
  if (!token.has_value() || token->is_nil()) {
    return malformed("authority claim token is not a valid uuid");
  }
  claim.token = *token;
  return claim;
}

Json to_json(const ComponentId& id) { return Json(id.str()); }
Json to_json(const ShardId& id) { return Json(id.str()); }

Result<ComponentId> component_id_from_json(const Json& value) {
  return name_from_json<ComponentTag, NamePolicy<ComponentTag>>(value, "component");
}

Result<ShardId> shard_id_from_json(const Json& value) {
  return name_from_json<ShardTag, NamePolicy<ShardTag>>(value, "shard");
}

}  // namespace fabric::evolution
