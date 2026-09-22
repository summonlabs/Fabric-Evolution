// Fabric Evolution — bounded, deterministic JSON value model.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// This is first-party code: the runtime has no third-party dependencies, and
// every persisted record, wire message and CLI response is encoded with this
// model. The encoder is canonical (object keys are emitted in ascending
// byte order, integers without a fractional part, no insignificant
// whitespace), so a digest over an encoding is a digest over the value.

#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace fabric::evolution {

class Json {
 public:
  using Array = std::vector<Json>;
  using Object = std::map<std::string, Json, std::less<>>;
  using Storage = std::variant<std::nullptr_t, bool, std::int64_t, std::uint64_t, double, std::string,
                               Array, Object>;

  Json() noexcept;
  Json(std::nullptr_t) noexcept;
  Json(bool value) noexcept;
  Json(int value) noexcept;
  Json(long value) noexcept;
  Json(long long value) noexcept;
  Json(unsigned int value) noexcept;
  Json(unsigned long value) noexcept;
  Json(unsigned long long value) noexcept;
  Json(double value) noexcept;
  Json(const char* value);
  Json(std::string value);
  Json(std::string_view value);
  Json(Array value);
  Json(Object value);

  [[nodiscard]] static Json array();
  [[nodiscard]] static Json array(std::initializer_list<Json> values);
  [[nodiscard]] static Json object();
  [[nodiscard]] static Json object(std::initializer_list<std::pair<const std::string, Json>> values);

  [[nodiscard]] bool is_null() const noexcept;
  [[nodiscard]] bool is_bool() const noexcept;
  [[nodiscard]] bool is_int() const noexcept;
  [[nodiscard]] bool is_uint() const noexcept;
  [[nodiscard]] bool is_number() const noexcept;
  [[nodiscard]] bool is_string() const noexcept;
  [[nodiscard]] bool is_array() const noexcept;
  [[nodiscard]] bool is_object() const noexcept;

  [[nodiscard]] std::optional<bool> try_bool() const noexcept;
  [[nodiscard]] std::optional<std::int64_t> try_i64() const noexcept;
  [[nodiscard]] std::optional<std::uint64_t> try_u64() const noexcept;
  [[nodiscard]] std::optional<double> try_f64() const noexcept;
  [[nodiscard]] const std::string* try_string() const noexcept;
  [[nodiscard]] const Array* try_array() const noexcept;
  [[nodiscard]] Array* try_array() noexcept;
  [[nodiscard]] const Object* try_object() const noexcept;
  [[nodiscard]] Object* try_object() noexcept;

  // Object access. Missing keys yield nullptr / no-op, never an exception.
  [[nodiscard]] const Json* find(std::string_view key) const noexcept;
  [[nodiscard]] bool has(std::string_view key) const noexcept;
  Json& set(std::string key, Json value);
  void erase(std::string_view key);

  // Array access.
  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] const Json& at(std::size_t index) const;
  Json& push_back(Json value);

  // Canonical serialization. Non-finite doubles are encoded as null.
  void dump_to(std::string& out) const;
  [[nodiscard]] std::string dump() const;
  [[nodiscard]] std::string dump_pretty() const;

  friend bool operator==(const Json& lhs, const Json& rhs) noexcept;

 private:
  void dump_value(std::string& out, bool pretty, int indent) const;

  Storage value_;
};

// Resource limits applied while parsing untrusted input.
struct JsonParseLimits {
  std::size_t max_bytes = 4U * 1024U * 1024U;
  std::size_t max_depth = 64;
  std::size_t max_string_bytes = 1024U * 1024U;
  std::size_t max_container_entries = 65536;
};

struct JsonParseResult {
  std::optional<Json> value;
  std::string error;
  std::size_t error_offset = 0;

  [[nodiscard]] bool ok() const noexcept { return value.has_value(); }
};

[[nodiscard]] JsonParseResult parse_json(std::string_view text);
[[nodiscard]] JsonParseResult parse_json(std::string_view text, const JsonParseLimits& limits);

// Convenience typed readers used by record decoders. They return nullopt when
// the key is absent or has the wrong type; the caller decides whether that is a
// default or a malformed record.
[[nodiscard]] std::optional<std::string> json_string(const Json& object, std::string_view key);
[[nodiscard]] std::optional<std::int64_t> json_i64(const Json& object, std::string_view key);
[[nodiscard]] std::optional<std::uint64_t> json_u64(const Json& object, std::string_view key);
[[nodiscard]] std::optional<bool> json_bool(const Json& object, std::string_view key);
[[nodiscard]] std::string json_string_or(const Json& object, std::string_view key, std::string fallback);
[[nodiscard]] std::int64_t json_i64_or(const Json& object, std::string_view key, std::int64_t fallback);
[[nodiscard]] std::uint64_t json_u64_or(const Json& object, std::string_view key, std::uint64_t fallback);
[[nodiscard]] bool json_bool_or(const Json& object, std::string_view key, bool fallback);

}  // namespace fabric::evolution
