// Fabric Evolution — version identities (implementation).
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric/evolution/versions.hpp"

#include <algorithm>
#include <charconv>
#include <vector>

namespace fabric::evolution {
namespace {

[[nodiscard]] bool is_build_char(char c) noexcept {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
         c == '-' || c == '_';
}

[[nodiscard]] bool parse_component(std::string_view text, std::uint16_t& out) noexcept {
  if (text.empty() || text.size() > 5) {
    return false;
  }
  std::uint32_t value = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
    return false;
  }
  if (value > 65535U) {
    return false;
  }
  out = static_cast<std::uint16_t>(value);
  return true;
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

}  // namespace

SoftwareVersion SoftwareVersion::of(std::uint16_t major, std::uint16_t minor, std::uint16_t patch,
                                    std::string build) {
  SoftwareVersion version(major, minor, patch);
  version.build_ = std::move(build);
  return version;
}

std::optional<SoftwareVersion> SoftwareVersion::parse(std::string_view text) {
  if (text.size() > 96) {
    return std::nullopt;
  }
  std::string_view numeric = text;
  std::string_view build;
  const std::size_t plus = text.find('+');
  if (plus != std::string_view::npos) {
    numeric = text.substr(0, plus);
    build = text.substr(plus + 1);
    if (build.empty() || !std::all_of(build.begin(), build.end(), is_build_char)) {
      return std::nullopt;
    }
  }
  const std::vector<std::string_view> parts = split(numeric, '.');
  if (parts.size() != 3) {
    return std::nullopt;
  }
  SoftwareVersion version;
  if (!parse_component(parts[0], version.major_) || !parse_component(parts[1], version.minor_) ||
      !parse_component(parts[2], version.patch_)) {
    return std::nullopt;
  }
  version.build_ = std::string(build);
  return version;
}

std::string SoftwareVersion::to_string() const {
  std::string out = std::to_string(major_) + "." + std::to_string(minor_) + "." + std::to_string(patch_);
  if (!build_.empty()) {
    out.push_back('+');
    out += build_;
  }
  return out;
}

bool SoftwareVersion::same_release(const SoftwareVersion& other) const noexcept {
  return major_ == other.major_ && minor_ == other.minor_ && patch_ == other.patch_;
}

std::strong_ordering operator<=>(const SoftwareVersion& lhs, const SoftwareVersion& rhs) noexcept {
  if (const auto cmp = lhs.major_ <=> rhs.major_; cmp != 0) {
    return cmp;
  }
  if (const auto cmp = lhs.minor_ <=> rhs.minor_; cmp != 0) {
    return cmp;
  }
  if (const auto cmp = lhs.patch_ <=> rhs.patch_; cmp != 0) {
    return cmp;
  }
  return lhs.build_ <=> rhs.build_;
}

std::optional<ProtocolVersion> ProtocolVersion::parse(std::string_view text) {
  if (text.size() > 32) {
    return std::nullopt;
  }
  const std::vector<std::string_view> parts = split(text, '.');
  if (parts.size() != 2) {
    return std::nullopt;
  }
  ProtocolVersion version;
  if (!parse_component(parts[0], version.major_) || !parse_component(parts[1], version.minor_)) {
    return std::nullopt;
  }
  return version;
}

ProtocolVersion ProtocolVersion::negotiate(const ProtocolVersion& other) const noexcept {
  return ProtocolVersion(major_, std::min(minor_, other.minor_));
}

std::string ProtocolVersion::to_string() const {
  return std::to_string(major_) + "." + std::to_string(minor_);
}

}  // namespace fabric::evolution
