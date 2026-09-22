// Fabric Evolution — content digests and frame checksums (implementation).
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric/evolution/digest.hpp"

#include <cstring>

namespace fabric::evolution {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
    0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
    0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
    0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
    0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
    0xc67178f2U};

[[nodiscard]] constexpr std::uint32_t rotr(std::uint32_t value, unsigned count) noexcept {
  return (value >> count) | (value << (32U - count));
}

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

[[nodiscard]] std::array<std::uint32_t, 256> build_crc_table() noexcept {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t index = 0; index < 256; ++index) {
    std::uint32_t value = index;
    for (int bit = 0; bit < 8; ++bit) {
      value = (value & 1U) != 0U ? (0xEDB88320U ^ (value >> 1U)) : (value >> 1U);
    }
    table[index] = value;
  }
  return table;
}

}  // namespace

std::string Digest::to_hex() const {
  std::string out;
  out.reserve(64);
  for (std::uint8_t byte : bytes_) {
    out.push_back(kHexDigits[(byte >> 4U) & 0x0FU]);
    out.push_back(kHexDigits[byte & 0x0FU]);
  }
  return out;
}

std::string Digest::to_short_hex() const {
  std::string out = to_hex();
  out.resize(16);
  return out;
}

std::optional<Digest> Digest::from_hex(std::string_view text) {
  if (text.size() != 64) {
    return std::nullopt;
  }
  bytes_type bytes{};
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    const int high = hex_value(text[index * 2]);
    const int low = hex_value(text[index * 2 + 1]);
    if (high < 0 || low < 0) {
      return std::nullopt;
    }
    bytes[index] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return Digest(bytes);
}

void Sha256::reset() noexcept {
  state_ = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
            0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
  buffer_.fill(0);
  total_bytes_ = 0;
  buffer_used_ = 0;
}

void Sha256::compress(const std::uint8_t* block) noexcept {
  std::array<std::uint32_t, 64> schedule{};
  for (std::size_t index = 0; index < 16; ++index) {
    const std::size_t offset = index * 4;
    schedule[index] = (static_cast<std::uint32_t>(block[offset]) << 24U) |
                      (static_cast<std::uint32_t>(block[offset + 1]) << 16U) |
                      (static_cast<std::uint32_t>(block[offset + 2]) << 8U) |
                      static_cast<std::uint32_t>(block[offset + 3]);
  }
  for (std::size_t index = 16; index < 64; ++index) {
    const std::uint32_t s0 = rotr(schedule[index - 15], 7) ^ rotr(schedule[index - 15], 18) ^
                             (schedule[index - 15] >> 3U);
    const std::uint32_t s1 = rotr(schedule[index - 2], 17) ^ rotr(schedule[index - 2], 19) ^
                             (schedule[index - 2] >> 10U);
    schedule[index] = schedule[index - 16] + s0 + schedule[index - 7] + s1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (std::size_t index = 0; index < 64; ++index) {
    const std::uint32_t sigma1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const std::uint32_t choose = (e & f) ^ ((~e) & g);
    const std::uint32_t temp1 = h + sigma1 + choose + kRoundConstants[index] + schedule[index];
    const std::uint32_t sigma0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = sigma0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(std::span<const std::uint8_t> data) noexcept {
  total_bytes_ += static_cast<std::uint64_t>(data.size());
  std::size_t offset = 0;
  if (buffer_used_ != 0) {
    while (offset < data.size() && buffer_used_ < buffer_.size()) {
      buffer_[buffer_used_] = data[offset];
      ++buffer_used_;
      ++offset;
    }
    if (buffer_used_ == buffer_.size()) {
      compress(buffer_.data());
      buffer_used_ = 0;
    }
  }
  while (data.size() - offset >= buffer_.size()) {
    compress(data.data() + offset);
    offset += buffer_.size();
  }
  while (offset < data.size()) {
    buffer_[buffer_used_] = data[offset];
    ++buffer_used_;
    ++offset;
  }
}

void Sha256::update(std::string_view data) noexcept {
  update(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(data.data()), data.size()));
}

Digest Sha256::finalize() noexcept {
  const std::uint64_t bit_length = total_bytes_ * 8ULL;
  const std::uint8_t pad = 0x80U;
  update(std::span<const std::uint8_t>(&pad, 1));
  const std::uint8_t zero = 0x00U;
  while (buffer_used_ != 56) {
    update(std::span<const std::uint8_t>(&zero, 1));
  }
  std::array<std::uint8_t, 8> length_bytes{};
  for (std::size_t index = 0; index < 8; ++index) {
    length_bytes[7 - index] = static_cast<std::uint8_t>((bit_length >> (8U * index)) & 0xFFU);
  }
  update(std::span<const std::uint8_t>(length_bytes.data(), length_bytes.size()));

  Digest::bytes_type out{};
  for (std::size_t index = 0; index < state_.size(); ++index) {
    out[index * 4] = static_cast<std::uint8_t>((state_[index] >> 24U) & 0xFFU);
    out[index * 4 + 1] = static_cast<std::uint8_t>((state_[index] >> 16U) & 0xFFU);
    out[index * 4 + 2] = static_cast<std::uint8_t>((state_[index] >> 8U) & 0xFFU);
    out[index * 4 + 3] = static_cast<std::uint8_t>(state_[index] & 0xFFU);
  }
  return Digest(out);
}

Digest sha256(std::span<const std::uint8_t> data) noexcept {
  Sha256 hasher;
  hasher.update(data);
  return hasher.finalize();
}

Digest sha256(std::string_view data) noexcept {
  Sha256 hasher;
  hasher.update(data);
  return hasher.finalize();
}

std::uint32_t crc32(std::span<const std::uint8_t> data) noexcept {
  static const std::array<std::uint32_t, 256> table = build_crc_table();
  std::uint32_t crc = 0xFFFFFFFFU;
  for (std::uint8_t byte : data) {
    crc = table[(crc ^ byte) & 0xFFU] ^ (crc >> 8U);
  }
  return crc ^ 0xFFFFFFFFU;
}

std::uint32_t crc32(std::string_view data) noexcept {
  return crc32(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(data.data()), data.size()));
}

}  // namespace fabric::evolution
