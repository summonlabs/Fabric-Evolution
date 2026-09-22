// Fabric Evolution — strong identity primitives (implementation).
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric/evolution/strong_types.hpp"

namespace fabric::evolution {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

[[nodiscard]] int hex_value(char c) noexcept {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

}  // namespace

std::string Uuid128::to_compact_string() const {
  std::string out;
  out.reserve(32);
  for (std::uint8_t byte : bytes_) {
    out.push_back(kHexDigits[(byte >> 4U) & 0x0FU]);
    out.push_back(kHexDigits[byte & 0x0FU]);
  }
  return out;
}

std::string Uuid128::to_string() const {
  const std::string compact = to_compact_string();
  std::string out;
  out.reserve(36);
  out.append(compact, 0, 8);
  out.push_back('-');
  out.append(compact, 8, 4);
  out.push_back('-');
  out.append(compact, 12, 4);
  out.push_back('-');
  out.append(compact, 16, 4);
  out.push_back('-');
  out.append(compact, 20, 12);
  return out;
}

std::optional<Uuid128> Uuid128::parse(std::string_view text) {
  std::array<std::uint8_t, 32> nibbles{};
  std::size_t count = 0;
  for (char c : text) {
    if (c == '-') {
      continue;
    }
    const int value = hex_value(c);
    if (value < 0) {
      return std::nullopt;
    }
    if (count >= nibbles.size()) {
      return std::nullopt;
    }
    nibbles[count] = static_cast<std::uint8_t>(value);
    ++count;
  }
  if (count != nibbles.size()) {
    return std::nullopt;
  }
  bytes_type bytes{};
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    bytes[index] = static_cast<std::uint8_t>((nibbles[index * 2] << 4U) | nibbles[index * 2 + 1]);
  }
  return Uuid128(bytes);
}

}  // namespace fabric::evolution
