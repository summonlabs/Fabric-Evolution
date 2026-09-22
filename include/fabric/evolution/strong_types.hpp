// Fabric Evolution — strong identity primitives.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace fabric::evolution {

// ---------------------------------------------------------------------------
// Tagged<Tag, Rep>
//
// A distinct, non-interchangeable integral identity. Two Tagged types with
// different tags are unrelated types: a Generation can never be assigned to an
// EpochNumber, and neither decays to a raw integer. Value 0 is reserved as the
// "invalid / unset" sentinel; the domain never uses 0 as a live identity.
// ---------------------------------------------------------------------------
template <class Tag, class Rep = std::uint64_t>
class Tagged {
 public:
  using rep_type = Rep;
  using tag_type = Tag;

  constexpr Tagged() noexcept = default;
  constexpr explicit Tagged(Rep value) noexcept : value_(value) {}

  [[nodiscard]] static constexpr Tagged from_value(Rep value) noexcept { return Tagged(value); }
  [[nodiscard]] static constexpr Tagged invalid() noexcept { return Tagged(); }

  [[nodiscard]] constexpr Rep value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_valid() const noexcept { return value_ != Rep{0}; }
  [[nodiscard]] constexpr bool is_invalid() const noexcept { return value_ == Rep{0}; }

  // Checked successor. Returns nullopt instead of wrapping at the type limit.
  [[nodiscard]] constexpr std::optional<Tagged> next() const noexcept {
    if (value_ == std::numeric_limits<Rep>::max()) {
      return std::nullopt;
    }
    return Tagged(static_cast<Rep>(value_ + Rep{1}));
  }

  friend constexpr bool operator==(Tagged, Tagged) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(Tagged, Tagged) noexcept = default;

 private:
  Rep value_{};
};

template <class Tag, class Rep>
[[nodiscard]] std::string to_string(Tagged<Tag, Rep> value) {
  return std::to_string(value.value());
}

// ---------------------------------------------------------------------------
// Uuid128 — 128-bit opaque identifier used for boot/incarnation identity,
// authority tokens and request correlation. Unlike a counter it cannot be
// guessed or replayed into an older generation.
// ---------------------------------------------------------------------------
class Uuid128 {
 public:
  using bytes_type = std::array<std::uint8_t, 16>;

  constexpr Uuid128() noexcept = default;
  constexpr explicit Uuid128(bytes_type bytes) noexcept : bytes_(bytes) {}

  [[nodiscard]] static Uuid128 nil() noexcept { return Uuid128{}; }

  [[nodiscard]] constexpr const bytes_type& bytes() const noexcept { return bytes_; }
  [[nodiscard]] constexpr bool is_nil() const noexcept {
    for (std::uint8_t b : bytes_) {
      if (b != 0) {
        return false;
      }
    }
    return true;
  }

  // Canonical lowercase hyphenated form: 8-4-4-4-12 hex digits.
  [[nodiscard]] std::string to_string() const;
  // Compact 32 hex digit form (no hyphens), used in persisted records.
  [[nodiscard]] std::string to_compact_string() const;
  [[nodiscard]] static std::optional<Uuid128> parse(std::string_view text);

  friend constexpr bool operator==(Uuid128, Uuid128) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(Uuid128, Uuid128) noexcept = default;

 private:
  bytes_type bytes_{};
};

// A tagged Uuid128: AuthorityToken, IncarnationUuid, RequestId and friends are
// all 128-bit, and none of them may be substituted for another.
template <class Tag>
class TaggedUuid {
 public:
  using tag_type = Tag;

  constexpr TaggedUuid() noexcept = default;
  constexpr explicit TaggedUuid(Uuid128 value) noexcept : value_(value) {}

  [[nodiscard]] static constexpr TaggedUuid from_value(Uuid128 value) noexcept {
    return TaggedUuid(value);
  }
  [[nodiscard]] static constexpr TaggedUuid nil() noexcept { return TaggedUuid{}; }

  [[nodiscard]] constexpr const Uuid128& value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_nil() const noexcept { return value_.is_nil(); }
  [[nodiscard]] constexpr bool is_valid() const noexcept { return !value_.is_nil(); }

  [[nodiscard]] std::string to_string() const { return value_.to_string(); }
  [[nodiscard]] std::string to_compact_string() const { return value_.to_compact_string(); }
  [[nodiscard]] static std::optional<TaggedUuid> parse(std::string_view text) {
    auto parsed = Uuid128::parse(text);
    if (!parsed.has_value()) {
      return std::nullopt;
    }
    return TaggedUuid(*parsed);
  }

  friend constexpr bool operator==(TaggedUuid, TaggedUuid) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(TaggedUuid, TaggedUuid) noexcept = default;

 private:
  Uuid128 value_{};
};

// ---------------------------------------------------------------------------
// TaggedName<Tag, Policy>
//
// A validated, non-interchangeable textual identity (component, shard,
// campaign, migration step). Validation is total and happens at construction,
// so an invalid name cannot exist in the domain model.
// ---------------------------------------------------------------------------
template <class Tag>
struct NamePolicy {
  static constexpr std::size_t max_length = 64;
  static constexpr bool allow_leading_separator = false;

  [[nodiscard]] static constexpr bool is_valid_char(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '.' || c == '_' || c == '-';
  }

  [[nodiscard]] static constexpr bool is_separator(char c) noexcept {
    return c == '.' || c == '_' || c == '-';
  }
};

template <class Tag, class Policy = NamePolicy<Tag>>
class TaggedName {
 public:
  using tag_type = Tag;
  using policy_type = Policy;

  TaggedName() = default;

  [[nodiscard]] static std::optional<TaggedName> parse(std::string_view text) {
    if (!is_valid(text)) {
      return std::nullopt;
    }
    return TaggedName(std::string(text), TrustTag{});
  }

  // Trusted construction for values already validated by the same policy.
  [[nodiscard]] static TaggedName unchecked(std::string text) {
    return TaggedName(std::move(text), TrustTag{});
  }

  [[nodiscard]] static bool is_valid(std::string_view text) noexcept {
    if (text.empty() || text.size() > Policy::max_length) {
      return false;
    }
    if (!Policy::allow_leading_separator && Policy::is_separator(text.front())) {
      return false;
    }
    for (char c : text) {
      if (!Policy::is_valid_char(c)) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] bool is_valid() const noexcept { return !value_.empty(); }
  [[nodiscard]] const std::string& str() const noexcept { return value_; }
  [[nodiscard]] std::string_view view() const noexcept { return value_; }

  friend bool operator==(const TaggedName&, const TaggedName&) noexcept = default;
  friend std::strong_ordering operator<=>(const TaggedName& lhs, const TaggedName& rhs) noexcept {
    return lhs.value_ <=> rhs.value_;
  }

 private:
  struct TrustTag {};

  TaggedName(std::string text, TrustTag) : value_(std::move(text)) {}

  std::string value_;
};

template <class Tag, class Policy>
[[nodiscard]] std::string to_string(const TaggedName<Tag, Policy>& name) {
  return name.str();
}

// ---------------------------------------------------------------------------
// Fixed-width little-endian byte encoding helpers. Persisted and wire formats
// are byte-exact so that digests are stable across compilers and platforms.
// ---------------------------------------------------------------------------
inline void append_u8(std::string& out, std::uint8_t value) {
  out.push_back(static_cast<char>(value));
}

inline void append_u16_le(std::string& out, std::uint16_t value) {
  append_u8(out, static_cast<std::uint8_t>(value & 0xFFU));
  append_u8(out, static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

inline void append_u32_le(std::string& out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    append_u8(out, static_cast<std::uint8_t>((value >> shift) & 0xFFU));
  }
}

inline void append_u64_le(std::string& out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    append_u8(out, static_cast<std::uint8_t>((value >> shift) & 0xFFU));
  }
}

[[nodiscard]] inline std::uint16_t read_u16_le(const std::uint8_t* data) noexcept {
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[0]) |
                                    static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[1]) << 8U));
}

[[nodiscard]] inline std::uint32_t read_u32_le(const std::uint8_t* data) noexcept {
  std::uint32_t value = 0;
  for (int index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(data[index]) << (8U * static_cast<unsigned>(index));
  }
  return value;
}

[[nodiscard]] inline std::uint64_t read_u64_le(const std::uint8_t* data) noexcept {
  std::uint64_t value = 0;
  for (int index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(data[index]) << (8U * static_cast<unsigned>(index));
  }
  return value;
}

// ---------------------------------------------------------------------------
// Checked size arithmetic for externally derived sizes and counts.
// ---------------------------------------------------------------------------
[[nodiscard]] inline std::optional<std::size_t> checked_add(std::size_t lhs, std::size_t rhs) noexcept {
  if (lhs > std::numeric_limits<std::size_t>::max() - rhs) {
    return std::nullopt;
  }
  return lhs + rhs;
}

[[nodiscard]] inline std::optional<std::size_t> checked_mul(std::size_t lhs, std::size_t rhs) noexcept {
  if (lhs != 0 && rhs > std::numeric_limits<std::size_t>::max() / lhs) {
    return std::nullopt;
  }
  return lhs * rhs;
}

}  // namespace fabric::evolution
