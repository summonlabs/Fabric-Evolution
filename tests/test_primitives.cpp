// Fabric Evolution — primitive type tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <string>
#include <vector>

#include "fabric/evolution/digest.hpp"
#include "fabric/evolution/features.hpp"
#include "fabric/evolution/ids.hpp"
#include "fabric/evolution/json.hpp"
#include "fabric/evolution/status.hpp"
#include "fabric/evolution/versions.hpp"
#include "support/test_harness.hpp"

namespace fabric::evolution {
namespace {

using test::Failure;

}  // namespace

FABRIC_TEST(digest, sha256_known_vectors) {
  FABRIC_CHECK_EQ(sha256("").to_hex(),
                  std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  FABRIC_CHECK_EQ(sha256("abc").to_hex(),
                  std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  FABRIC_CHECK_EQ(
      sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq").to_hex(),
      std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
  // 1,000,000 'a' characters, fed one byte at a time to exercise buffering.
  Sha256 hasher;
  const std::string one_million(1000000, 'a');
  hasher.update(one_million);
  FABRIC_CHECK_EQ(hasher.finalize().to_hex(),
                  std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

FABRIC_TEST(digest, sha256_incremental_matches_oneshot) {
  std::string payload;
  for (int index = 0; index < 5000; ++index) {
    payload.push_back(static_cast<char>((index * 37) % 251));
  }
  const Digest one_shot = sha256(payload);
  for (std::size_t chunk = 1; chunk <= 130; chunk += 7) {
    Sha256 hasher;
    std::size_t offset = 0;
    while (offset < payload.size()) {
      const std::size_t length = std::min(chunk, payload.size() - offset);
      hasher.update(std::string_view(payload).substr(offset, length));
      offset += length;
    }
    FABRIC_CHECK(hasher.finalize() == one_shot);
  }
}

FABRIC_TEST(digest, crc32_known_vectors) {
  FABRIC_CHECK_EQ(crc32("123456789"), 0xCBF43926U);
  FABRIC_CHECK_EQ(crc32(""), 0x00000000U);
  FABRIC_CHECK_EQ(crc32("The quick brown fox jumps over the lazy dog"), 0x414FA339U);
}

FABRIC_TEST(digest, hex_round_trip) {
  const Digest digest = sha256("fabric evolution");
  const auto parsed = Digest::from_hex(digest.to_hex());
  FABRIC_CHECK(parsed.has_value());
  FABRIC_CHECK(*parsed == digest);
  FABRIC_CHECK(!Digest::from_hex("abc").has_value());
  FABRIC_CHECK(!Digest::from_hex(std::string(64, 'z')).has_value());
}

FABRIC_TEST(strong_types, uuid_round_trip) {
  const Uuid128::bytes_type bytes{0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
                                  0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10};
  const Uuid128 value(bytes);
  FABRIC_CHECK_EQ(value.to_string(), std::string("01234567-89ab-cdef-fedc-ba9876543210"));
  FABRIC_CHECK_EQ(value.to_compact_string(), std::string("0123456789abcdeffedcba9876543210"));
  const auto parsed = Uuid128::parse(value.to_string());
  FABRIC_CHECK(parsed.has_value());
  FABRIC_CHECK(*parsed == value);
  FABRIC_CHECK(Uuid128::parse("0123456789abcdeffedcba98765432").has_value() == false);
  FABRIC_CHECK(Uuid128::parse("0123456789abcdeffedcba98765432gg").has_value() == false);
  FABRIC_CHECK(Uuid128::nil().is_nil());
}

FABRIC_TEST(strong_types, tagged_names_validate) {
  FABRIC_CHECK(ComponentId::parse("shard-0-a").has_value());
  FABRIC_CHECK(ComponentId::parse("").has_value() == false);
  FABRIC_CHECK(ComponentId::parse("-leading").has_value() == false);
  FABRIC_CHECK(ComponentId::parse("has space").has_value() == false);
  FABRIC_CHECK(ComponentId::parse(std::string(65, 'a')).has_value() == false);
  FABRIC_CHECK(ComponentId::parse(std::string(64, 'a')).has_value());
  FABRIC_CHECK(ComponentId::parse("slash/inside").has_value() == false);
}

FABRIC_TEST(strong_types, tagged_increment_checks_overflow) {
  const Tagged<GenerationTag, std::uint8_t> almost = Tagged<GenerationTag, std::uint8_t>::from_value(254);
  const auto next = almost.next();
  FABRIC_CHECK(next.has_value());
  FABRIC_CHECK_EQ(next->value(), static_cast<std::uint8_t>(255));
  FABRIC_CHECK(next->next().has_value() == false);
}

FABRIC_TEST(strong_types, checked_arithmetic_rejects_overflow) {
  FABRIC_CHECK(checked_add(1, 2).has_value());
  FABRIC_CHECK_EQ(*checked_add(1, 2), static_cast<std::size_t>(3));
  FABRIC_CHECK(checked_add(std::numeric_limits<std::size_t>::max(), 1).has_value() == false);
  FABRIC_CHECK(checked_mul(4, 5).has_value());
  FABRIC_CHECK(checked_mul(std::numeric_limits<std::size_t>::max(), 2).has_value() == false);
  FABRIC_CHECK(checked_mul(0, std::numeric_limits<std::size_t>::max()).has_value());
}

FABRIC_TEST(versions, software_version_parse_and_order) {
  const auto version = SoftwareVersion::parse("1.2.3");
  FABRIC_CHECK(version.has_value());
  FABRIC_CHECK_EQ(version->major(), static_cast<std::uint16_t>(1));
  FABRIC_CHECK_EQ(version->to_string(), std::string("1.2.3"));

  const auto with_build = SoftwareVersion::parse("1.2.3+rc1");
  FABRIC_CHECK(with_build.has_value());
  FABRIC_CHECK_EQ(with_build->to_string(), std::string("1.2.3+rc1"));

  FABRIC_CHECK(SoftwareVersion::parse("1.2").has_value() == false);
  FABRIC_CHECK(SoftwareVersion::parse("1.2.3.4").has_value() == false);
  FABRIC_CHECK(SoftwareVersion::parse("1.2.x").has_value() == false);
  FABRIC_CHECK(SoftwareVersion::parse("1.2.3+bad build").has_value() == false);
  FABRIC_CHECK(SoftwareVersion::parse("70000.2.3").has_value() == false);

  FABRIC_CHECK(SoftwareVersion::of(1, 2, 3) < SoftwareVersion::of(1, 2, 4));
  FABRIC_CHECK(SoftwareVersion::of(1, 2, 3) < SoftwareVersion::of(1, 3, 0));
  FABRIC_CHECK(SoftwareVersion::of(1, 2, 3, "a") < SoftwareVersion::of(1, 2, 3, "b"));
  FABRIC_CHECK(SoftwareVersion::of(1, 2, 3, "a").same_release(SoftwareVersion::of(1, 2, 3, "b")));
}

FABRIC_TEST(versions, protocol_negotiation_takes_the_lower_minor) {
  FABRIC_CHECK(ProtocolVersion::of(1, 5).compatible_with(ProtocolVersion::of(1, 2)));
  FABRIC_CHECK(!ProtocolVersion::of(2, 0).compatible_with(ProtocolVersion::of(1, 9)));
  FABRIC_CHECK_EQ(ProtocolVersion::of(1, 5).negotiate(ProtocolVersion::of(1, 2)),
                  ProtocolVersion::of(1, 2));
  FABRIC_CHECK_EQ(ProtocolVersion::of(1, 2).negotiate(ProtocolVersion::of(1, 2)),
                  ProtocolVersion::of(1, 2));
  const auto parsed = ProtocolVersion::parse("3.11");
  FABRIC_CHECK(parsed.has_value());
  FABRIC_CHECK_EQ(parsed->to_string(), std::string("3.11"));
  FABRIC_CHECK(ProtocolVersion::parse("3").has_value() == false);
}

FABRIC_TEST(features, names_round_trip) {
  FeatureSet set;
  set.add(Feature::SnapshotTransfer).add(Feature::ForwardRecovery);
  const std::vector<std::string> names = set.names();
  FABRIC_CHECK_EQ(names.size(), static_cast<std::size_t>(2));
  FABRIC_CHECK_EQ(names[0], std::string("snapshot_transfer"));
  FABRIC_CHECK_EQ(names[1], std::string("forward_recovery"));
  std::string error;
  const auto parsed = FeatureSet::from_names(names, error);
  FABRIC_CHECK(parsed.has_value());
  FABRIC_CHECK(*parsed == set);
  const auto bad = FeatureSet::from_names({"no_such_feature"}, error);
  FABRIC_CHECK(!bad.has_value());
  FABRIC_CHECK_EQ(error, std::string("unknown feature: no_such_feature"));
}

FABRIC_TEST(features, set_algebra) {
  const FeatureSet a = FeatureSet::of(Feature::SnapshotTransfer).unite(FeatureSet::of(Feature::SchemaMigration));
  const FeatureSet b = FeatureSet::of(Feature::SnapshotTransfer);
  FABRIC_CHECK(a.contains_all(b));
  FABRIC_CHECK(!b.contains_all(a));
  FABRIC_CHECK_EQ(a.intersect(b), b);
  FABRIC_CHECK(a.intersects(b));
  FABRIC_CHECK(!a.without(b).contains(Feature::SnapshotTransfer));
  FABRIC_CHECK(a.without(b).contains(Feature::SchemaMigration));
}

FABRIC_TEST(json, canonical_encoding_sorts_object_keys) {
  Json value = Json::object();
  value.set("zulu", Json(1));
  value.set("alpha", Json(2));
  value.set("mike", Json(3));
  FABRIC_CHECK_EQ(value.dump(), std::string("{\"alpha\":2,\"mike\":3,\"zulu\":1}"));
}

FABRIC_TEST(json, round_trip_preserves_values) {
  const std::string text =
      "{\"a\":[1,2,3],\"b\":{\"c\":true,\"d\":null},\"e\":\"text\\nwith\\tescapes\","
      "\"f\":-17,\"g\":18446744073709551615}";
  const auto parsed = parse_json(text);
  FABRIC_CHECK(parsed.ok());
  FABRIC_CHECK_EQ(parsed.value->dump(), text);
  FABRIC_CHECK_EQ(parsed.value->find("g")->try_u64().value(), UINT64_MAX);
  FABRIC_CHECK(parsed.value->find("a")->is_array());
  FABRIC_CHECK_EQ(parsed.value->find("a")->size(), static_cast<std::size_t>(3));
  FABRIC_CHECK_EQ(parsed.value->find("a")->at(1).try_i64().value(), static_cast<std::int64_t>(2));
}

FABRIC_TEST(json, unicode_escapes_decode_to_utf8) {
  const auto parsed = parse_json("\"\\u0041\\u00e9\\u20ac\\ud83d\\ude00\"");
  FABRIC_CHECK(parsed.ok());
  FABRIC_CHECK_EQ(*parsed.value->try_string(), std::string("A\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80"));
}

FABRIC_TEST(json, adversarial_inputs_are_rejected) {
  const std::vector<std::string> bad = {
      "",
      "   ",
      "{",
      "}",
      "[1,2",
      "{\"a\":1,}",
      "{\"a\":1 \"b\":2}",
      "{\"a\":1,\"a\":2}",
      "{a:1}",
      "01",
      "-",
      "+1",
      ".5",
      "1.",
      "1e",
      "tru",
      "nul",
      "\"unterminated",
      "\"bad escape \\q\"",
      "\"\\u00\"",
      "\"\\ud800\"",
      "\"\\udc00\\ud800\"",
      "1 2",
      "[] []",
      "\"\x01\"",
      "NaN",
      "Infinity",
  };
  for (const std::string& text : bad) {
    const auto parsed = parse_json(text);
    if (parsed.ok()) {
      FABRIC_FAIL("expected rejection of: " + text);
    }
    FABRIC_CHECK(!parsed.error.empty());
  }
}

FABRIC_TEST(json, parse_limits_are_enforced) {
  JsonParseLimits limits;
  limits.max_depth = 3;
  FABRIC_CHECK(!parse_json("[[[[1]]]]", limits).ok());
  FABRIC_CHECK(parse_json("[[1]]", limits).ok());

  JsonParseLimits tiny;
  tiny.max_bytes = 8;
  FABRIC_CHECK(!parse_json("{\"aaaa\":1}", tiny).ok());

  JsonParseLimits shallow;
  shallow.max_container_entries = 2;
  FABRIC_CHECK(!parse_json("[1,2,3]", shallow).ok());
}

FABRIC_TEST(json, deeply_nested_input_does_not_crash) {
  std::string nested;
  for (int index = 0; index < 5000; ++index) {
    nested.push_back('[');
  }
  const auto parsed = parse_json(nested);
  FABRIC_CHECK(!parsed.ok());
  FABRIC_CHECK_EQ(parsed.error, std::string("maximum nesting depth exceeded"));
}

FABRIC_TEST(json, non_finite_doubles_encode_as_null) {
  Json value = Json::array();
  value.push_back(Json(std::numeric_limits<double>::quiet_NaN()));
  value.push_back(Json(std::numeric_limits<double>::infinity()));
  FABRIC_CHECK_EQ(value.dump(), std::string("[null,null]"));
  Json finite = Json::array();
  finite.push_back(Json(1.5));
  FABRIC_CHECK_EQ(finite.dump(), std::string("[1.5]"));
  Json integral_double = Json::array();
  integral_double.push_back(Json(2.0));
  FABRIC_CHECK_EQ(integral_double.dump(), std::string("[2.0]"));
}

FABRIC_TEST(status, rendering_and_json) {
  const Status ok;
  FABRIC_CHECK(ok.ok());
  FABRIC_CHECK_EQ(ok.to_string(), std::string("ok"));
  const Status failure = Status::error(ErrorCode::StaleGeneration, "stale", Json::object({{"presented", Json(2)}}));
  FABRIC_CHECK(failure.is_error());
  FABRIC_CHECK_EQ(failure.to_string(), std::string("stale_generation: stale"));
  FABRIC_CHECK_EQ(failure.to_json().dump(),
                  std::string("{\"code\":\"stale_generation\",\"detail\":{\"presented\":2},"
                              "\"message\":\"stale\"}"));
  FABRIC_CHECK(is_retryable(ErrorCode::NotReady));
  FABRIC_CHECK(!is_retryable(ErrorCode::StaleAuthority));
}

FABRIC_TEST(ids, incarnation_round_trip) {
  const auto component = ComponentId::parse("shard-0-primary");
  FABRIC_CHECK(component.has_value());
  const auto uuid = IncarnationUuid::parse("0123456789abcdeffedcba9876543210");
  FABRIC_CHECK(uuid.has_value());
  const IncarnationId incarnation(*component, *uuid, BootCounter::from_value(7));
  FABRIC_CHECK(incarnation.is_valid());
  const auto parsed = IncarnationId::parse(incarnation.to_string());
  FABRIC_CHECK(parsed.has_value());
  FABRIC_CHECK(*parsed == incarnation);
  FABRIC_CHECK(!IncarnationId::parse("shard-0-primary/7/nothex").has_value());
  FABRIC_CHECK(!IncarnationId::parse("shard-0-primary/0/0123456789abcdeffedcba9876543210").has_value());

  const Json encoded = to_json(incarnation);
  const auto decoded = incarnation_from_json(encoded);
  FABRIC_CHECK(decoded.ok());
  FABRIC_CHECK(decoded.value() == incarnation);
  FABRIC_CHECK(!incarnation_from_json(Json::object()).ok());
  FABRIC_CHECK(!incarnation_from_json(Json::object({{"component", Json("bad name")}})).ok());
}

FABRIC_TEST(ids, authority_claim_round_trip) {
  const auto shard = ShardId::parse("shard-0");
  FABRIC_CHECK(shard.has_value());
  const auto uuid = IncarnationUuid::parse("0123456789abcdeffedcba9876543210");
  FABRIC_CHECK(uuid.has_value());
  AuthorityClaim claim;
  claim.shard = *shard;
  claim.incarnation = IncarnationId(ComponentId::unchecked("node-a"), *uuid, BootCounter::from_value(1));
  claim.epoch = EpochNumber::from_value(3);
  claim.generation = Generation::from_value(9);
  claim.token = *AuthorityToken::parse("00112233445566778899aabbccddeeff");
  const auto decoded = authority_claim_from_json(to_json(claim));
  FABRIC_CHECK(decoded.ok());
  FABRIC_CHECK(decoded.value() == claim);
  FABRIC_CHECK(!authority_claim_from_json(Json::object()).ok());
  Json broken = to_json(claim);
  broken.set("generation", Json(0));
  FABRIC_CHECK(!authority_claim_from_json(broken).ok());
}

}  // namespace fabric::evolution
