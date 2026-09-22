// Fabric Evolution — bounded frame codec (implementation).
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric/evolution/framing.hpp"

#include <cstring>

#include "fabric/evolution/digest.hpp"
#include "fabric/evolution/json.hpp"
#include "fabric/evolution/strong_types.hpp"

namespace fabric::evolution {
namespace {

constexpr char kMagic[4] = {'F', 'X', 'F', '1'};
constexpr std::uint8_t kFrameVersion = 1;

}  // namespace

Result<std::string> encode_frame(std::string_view payload, std::uint8_t flags, const FrameLimits& limits) {
  if (payload.size() > limits.max_payload_bytes) {
    return Status::error(ErrorCode::BoundsExceeded, "frame payload exceeds the permitted size",
                         Json::object({{"payload_bytes", Json(static_cast<std::uint64_t>(payload.size()))},
                               {"limit", Json(static_cast<std::uint64_t>(limits.max_payload_bytes))}}));
  }
  std::string out;
  out.reserve(kFrameHeaderBytes + payload.size());
  out.append(kMagic, sizeof(kMagic));
  append_u8(out, kFrameVersion);
  append_u8(out, flags);
  append_u16_le(out, 0U);
  append_u32_le(out, static_cast<std::uint32_t>(payload.size()));
  const std::uint32_t header_crc =
      crc32(std::string_view(out.data(), out.size()));
  append_u32_le(out, header_crc);
  append_u32_le(out, crc32(payload));
  if (out.size() != kFrameHeaderBytes) {
    return Status::error(ErrorCode::Internal, "frame header size is inconsistent");
  }
  out.append(payload);
  return out;
}

void FrameDecoder::reset() noexcept { buffer_.clear(); }

Status FrameDecoder::finish() const {
  if (!buffer_.empty()) {
    return Status::error(ErrorCode::Malformed, "stream ended inside a frame",
                         Json::object({{"buffered_bytes", Json(static_cast<std::uint64_t>(buffer_.size()))}}));
  }
  return Status::success();
}

Status FrameDecoder::feed(std::string_view chunk, std::vector<Frame>& out) {
  if (chunk.empty()) {
    return Status::success();
  }
  const auto combined = checked_add(buffer_.size(), chunk.size());
  if (!combined.has_value() || *combined > limits_.max_buffered_bytes) {
    reset();
    return Status::error(ErrorCode::BoundsExceeded, "frame decoder buffer limit exceeded",
                         Json::object({{"limit", Json(static_cast<std::uint64_t>(limits_.max_buffered_bytes))}}));
  }
  buffer_.append(chunk);

  std::size_t consumed = 0;
  while (buffer_.size() - consumed >= kFrameHeaderBytes) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(buffer_.data() + consumed);
    if (std::memcmp(bytes, kMagic, sizeof(kMagic)) != 0) {
      reset();
      return Status::error(ErrorCode::ProtocolViolation, "frame magic mismatch");
    }
    const std::uint8_t version = bytes[4];
    if (version != kFrameVersion) {
      reset();
      return Status::error(ErrorCode::ProtocolViolation, "unsupported frame version",
                           Json::object({{"found", Json(static_cast<std::uint64_t>(version))},
                                 {"expected", Json(static_cast<std::uint64_t>(kFrameVersion))}}));
    }
    const std::uint32_t header_crc = read_u32_le(bytes + 12);
    if (crc32(std::string_view(buffer_.data() + consumed, 12)) != header_crc) {
      reset();
      return Status::error(ErrorCode::ProtocolViolation, "frame header checksum mismatch");
    }
    const std::uint32_t payload_bytes = read_u32_le(bytes + 8);
    if (payload_bytes > limits_.max_payload_bytes) {
      reset();
      return Status::error(ErrorCode::BoundsExceeded, "frame declares a payload beyond the permitted size",
                           Json::object({{"declared", Json(payload_bytes)},
                                 {"limit", Json(static_cast<std::uint64_t>(limits_.max_payload_bytes))}}));
    }
    const auto frame_end = checked_add(consumed, checked_add(kFrameHeaderBytes,
                                                             static_cast<std::size_t>(payload_bytes))
                                                      .value_or(SIZE_MAX));
    if (!frame_end.has_value()) {
      reset();
      return Status::error(ErrorCode::BoundsExceeded, "frame length arithmetic overflowed");
    }
    if (buffer_.size() < *frame_end) {
      break;  // wait for the rest of the payload
    }
    Frame frame;
    frame.flags = bytes[5];
    frame.payload.assign(buffer_.data() + consumed + kFrameHeaderBytes, payload_bytes);
    const std::uint32_t payload_crc = read_u32_le(bytes + 16);
    if (crc32(frame.payload) != payload_crc) {
      reset();
      return Status::error(ErrorCode::IntegrityFailure, "frame payload checksum mismatch");
    }
    out.push_back(std::move(frame));
    consumed = *frame_end;
  }

  if (consumed != 0) {
    buffer_.erase(0, consumed);
  }
  return Status::success();
}

}  // namespace fabric::evolution
