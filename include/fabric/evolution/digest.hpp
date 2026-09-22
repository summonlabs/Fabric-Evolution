// Fabric Evolution — content digests and frame checksums.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace fabric::evolution {

// SHA-256 over an explicit byte sequence. Used for manifest identity, snapshot
// integrity, migration evidence and persisted-state integrity. The algorithm is
// implemented in-tree so that digests are identical on every platform and no
// third-party dependency enters the trust boundary.
class Digest {
 public:
  using bytes_type = std::array<std::uint8_t, 32>;

  constexpr Digest() noexcept = default;
  constexpr explicit Digest(bytes_type bytes) noexcept : bytes_(bytes) {}

  [[nodiscard]] constexpr const bytes_type& bytes() const noexcept { return bytes_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept {
    for (std::uint8_t b : bytes_) {
      if (b != 0) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] std::string to_hex() const;
  [[nodiscard]] static std::optional<Digest> from_hex(std::string_view text);
  // Short form for logs and human output (first 16 hex digits).
  [[nodiscard]] std::string to_short_hex() const;

  friend constexpr bool operator==(Digest, Digest) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(Digest, Digest) noexcept = default;

 private:
  bytes_type bytes_{};
};

// Streaming SHA-256.
class Sha256 {
 public:
  Sha256() noexcept { reset(); }

  void reset() noexcept;
  void update(std::span<const std::uint8_t> data) noexcept;
  void update(std::string_view data) noexcept;
  [[nodiscard]] Digest finalize() noexcept;

 private:
  void compress(const std::uint8_t* block) noexcept;

  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, 64> buffer_{};
  std::uint64_t total_bytes_ = 0;
  std::size_t buffer_used_ = 0;
};

// One-shot helpers.
[[nodiscard]] Digest sha256(std::span<const std::uint8_t> data) noexcept;
[[nodiscard]] Digest sha256(std::string_view data) noexcept;

// CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320). Used as a cheap frame
// and record checksum in front of the stronger SHA-256 integrity checks.
[[nodiscard]] std::uint32_t crc32(std::span<const std::uint8_t> data) noexcept;
[[nodiscard]] std::uint32_t crc32(std::string_view data) noexcept;

}  // namespace fabric::evolution
