// Fabric Evolution — bounded frame codec for the runtime transport.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every byte that crosses a socket is a frame with a fixed header, a checked
// length, a header checksum and a payload checksum. The decoder is a strict
// state machine: it accepts arbitrarily fragmented input, rejects impossible
// lengths before allocating, and refuses to trust a header whose checksum does
// not verify.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "fabric/evolution/status.hpp"

namespace fabric::evolution {

inline constexpr std::size_t kFrameHeaderBytes = 20;

struct FrameLimits {
  std::size_t max_payload_bytes = 1024U * 1024U;
  std::size_t max_buffered_bytes = 4U * 1024U * 1024U;
};

enum class FrameFlags : std::uint8_t {
  None = 0,
  Response = 1U << 0U,
  Compressed = 1U << 1U,
};

struct Frame {
  std::string payload;
  std::uint8_t flags = 0;

  [[nodiscard]] bool has_flag(FrameFlags flag) const noexcept {
    return (flags & static_cast<std::uint8_t>(flag)) != 0;
  }
};

// Encodes one frame. Fails with BoundsExceeded when the payload is too large.
[[nodiscard]] Result<std::string> encode_frame(std::string_view payload, std::uint8_t flags,
                                               const FrameLimits& limits);

class FrameDecoder {
 public:
  FrameDecoder() = default;
  explicit FrameDecoder(FrameLimits limits) : limits_(limits) {}

  // Consumes a chunk of bytes and appends every complete frame to out.
  [[nodiscard]] Status feed(std::string_view chunk, std::vector<Frame>& out);
  // Signals end of stream; fails if a partial frame is buffered.
  [[nodiscard]] Status finish() const;
  void reset() noexcept;
  [[nodiscard]] std::size_t buffered_bytes() const noexcept { return buffer_.size(); }
  [[nodiscard]] const FrameLimits& limits() const noexcept { return limits_; }
  void set_limits(FrameLimits limits) noexcept { limits_ = limits; }

 private:
  std::string buffer_;
  FrameLimits limits_;
};

}  // namespace fabric::evolution
