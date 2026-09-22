// Fabric Evolution — software, protocol and schema version identities.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "fabric/evolution/strong_types.hpp"

namespace fabric::evolution {

// Semantic software version of a control-plane component build. Build metadata
// participates in ordering so that two distinct artifacts never compare equal.
class SoftwareVersion {
 public:
  constexpr SoftwareVersion() noexcept = default;
  constexpr SoftwareVersion(std::uint16_t major, std::uint16_t minor, std::uint16_t patch) noexcept
      : major_(major), minor_(minor), patch_(patch) {}

  [[nodiscard]] static SoftwareVersion of(std::uint16_t major, std::uint16_t minor,
                                          std::uint16_t patch, std::string build = {});
  [[nodiscard]] static std::optional<SoftwareVersion> parse(std::string_view text);

  [[nodiscard]] constexpr std::uint16_t major() const noexcept { return major_; }
  [[nodiscard]] constexpr std::uint16_t minor() const noexcept { return minor_; }
  [[nodiscard]] constexpr std::uint16_t patch() const noexcept { return patch_; }
  [[nodiscard]] const std::string& build() const noexcept { return build_; }
  [[nodiscard]] constexpr bool is_valid() const noexcept {
    return major_ != 0 || minor_ != 0 || patch_ != 0 || !build_.empty();
  }

  [[nodiscard]] std::string to_string() const;
  [[nodiscard]] bool same_release(const SoftwareVersion& other) const noexcept;

  friend bool operator==(const SoftwareVersion&, const SoftwareVersion&) noexcept = default;
  friend std::strong_ordering operator<=>(const SoftwareVersion& lhs,
                                          const SoftwareVersion& rhs) noexcept;

 private:
  std::uint16_t major_ = 0;
  std::uint16_t minor_ = 0;
  std::uint16_t patch_ = 0;
  std::string build_;
};

// Wire protocol version. Compatibility is decided by the major number; the
// minor number is the negotiation axis (effective minor = min of both sides).
class ProtocolVersion {
 public:
  constexpr ProtocolVersion() noexcept = default;
  constexpr ProtocolVersion(std::uint16_t major, std::uint16_t minor) noexcept
      : major_(major), minor_(minor) {}

  [[nodiscard]] static constexpr ProtocolVersion of(std::uint16_t major, std::uint16_t minor) noexcept {
    return ProtocolVersion(major, minor);
  }
  [[nodiscard]] static std::optional<ProtocolVersion> parse(std::string_view text);

  [[nodiscard]] constexpr std::uint16_t major() const noexcept { return major_; }
  [[nodiscard]] constexpr std::uint16_t minor() const noexcept { return minor_; }
  [[nodiscard]] constexpr bool is_valid() const noexcept { return major_ != 0; }

  [[nodiscard]] bool compatible_with(const ProtocolVersion& other) const noexcept {
    return major_ == other.major_;
  }
  // Deterministic negotiation: the lower minor wins, because a participant may
  // only use messages it can both emit and decode.
  [[nodiscard]] ProtocolVersion negotiate(const ProtocolVersion& other) const noexcept;

  [[nodiscard]] std::string to_string() const;

  friend constexpr bool operator==(ProtocolVersion, ProtocolVersion) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(ProtocolVersion lhs,
                                                    ProtocolVersion rhs) noexcept = default;

 private:
  std::uint16_t major_ = 0;
  std::uint16_t minor_ = 0;
};

struct SchemaTag;
// Schema version of persistent / replicated state. Monotonic; a downgrade is
// only legal when no irreversible boundary was crossed on the way up.
using SchemaVersion = Tagged<SchemaTag, std::uint32_t>;

}  // namespace fabric::evolution
