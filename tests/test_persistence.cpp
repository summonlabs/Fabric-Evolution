// Fabric Evolution — durable persistence and framing tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "fabric/evolution/framing.hpp"
#include "fabric/evolution/persistence.hpp"
#include "support/cluster.hpp"
#include "support/test_harness.hpp"

namespace fabric::evolution {
namespace {

void write_raw(const std::string& path, const std::string& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string read_raw(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  std::string contents;
  stream.seekg(0, std::ios::end);
  contents.resize(static_cast<std::size_t>(stream.tellg()));
  stream.seekg(0, std::ios::beg);
  if (!contents.empty()) {
    stream.read(contents.data(), static_cast<std::streamsize>(contents.size()));
  }
  return contents;
}

}  // namespace

FABRIC_TEST(persistence, integrity_file_round_trip) {
  test::TempDir directory("persistence");
  const std::string path = directory.sub("record.fxi");
  const std::string payload = "{\"hello\":\"fabric\"}";
  FABRIC_CHECK_OK(write_integrity_file(path, payload, false));
  IntegrityFileInfo info;
  auto read = read_integrity_file(path, &info);
  FABRIC_CHECK_OK(read);
  FABRIC_CHECK_EQ(read.value(), payload);
  FABRIC_CHECK_EQ(info.payload_digest, sha256(payload));
  FABRIC_CHECK_EQ(info.payload_bytes, static_cast<std::uint64_t>(payload.size()));
}

FABRIC_TEST(persistence, corrupt_payload_is_detected) {
  test::TempDir directory("persistence");
  const std::string path = directory.sub("record.fxi");
  FABRIC_CHECK_OK(write_integrity_file(path, "abcdefghij", false));
  std::string bytes = read_raw(path);
  bytes[bytes.size() - 3] = static_cast<char>(bytes[bytes.size() - 3] ^ 0x5A);
  write_raw(path, bytes);
  FABRIC_CHECK_ERR(read_integrity_file(path, nullptr), ErrorCode::IntegrityFailure);
}

FABRIC_TEST(persistence, truncated_and_oversized_records_are_detected) {
  test::TempDir directory("persistence");
  const std::string path = directory.sub("record.fxi");
  FABRIC_CHECK_OK(write_integrity_file(path, "abcdefghij", false));
  std::string bytes = read_raw(path);
  write_raw(path, bytes.substr(0, bytes.size() - 4));
  FABRIC_CHECK_ERR(read_integrity_file(path, nullptr), ErrorCode::IntegrityFailure);
  FABRIC_CHECK_ERR(read_integrity_file(directory.sub("missing.fxi"), nullptr), ErrorCode::IoFailure);
}

FABRIC_TEST(persistence, bad_magic_and_version_are_detected) {
  test::TempDir directory("persistence");
  const std::string path = directory.sub("record.fxi");
  FABRIC_CHECK_OK(write_integrity_file(path, "payload", false));
  std::string bytes = read_raw(path);
  std::string wrong_magic = bytes;
  wrong_magic[0] = 'X';
  write_raw(path, wrong_magic);
  FABRIC_CHECK_ERR(read_integrity_file(path, nullptr), ErrorCode::IntegrityFailure);

  std::string wrong_version = bytes;
  wrong_version[8] = static_cast<char>(9);
  write_raw(path, wrong_version);
  FABRIC_CHECK_ERR(read_integrity_file(path, nullptr), ErrorCode::IntegrityFailure);
}

FABRIC_TEST(persistence, backup_recovery_is_explicit_never_silent) {
  test::TempDir directory("persistence");
  const std::string path = directory.sub("record.fxi");
  FABRIC_CHECK_OK(write_integrity_file(path, "first", true));
  FABRIC_CHECK_OK(write_integrity_file(path, "second", true));
  // Damage the primary; the backup still holds the previous generation.
  std::string bytes = read_raw(path);
  bytes[bytes.size() - 1] = static_cast<char>(bytes[bytes.size() - 1] ^ 0x11);
  write_raw(path, bytes);

  FABRIC_CHECK_ERR(read_integrity_file(path, nullptr), ErrorCode::IntegrityFailure);
  FABRIC_CHECK_ERR(read_integrity_file_with_recovery(path, false), ErrorCode::IntegrityFailure);
  auto recovered = read_integrity_file_with_recovery(path, true);
  FABRIC_CHECK_OK(recovered);
  FABRIC_CHECK(recovered.value().from_backup);
  FABRIC_CHECK_EQ(recovered.value().payload, std::string("first"));
}

FABRIC_TEST(persistence, integrity_chain_records_the_previous_digest) {
  test::TempDir directory("persistence");
  const std::string path = directory.sub("record.fxi");
  FABRIC_CHECK_OK(write_integrity_file(path, "generation-one", false));
  IntegrityFileInfo first;
  FABRIC_CHECK_OK(read_integrity_file(path, &first));
  FABRIC_CHECK_OK(write_integrity_file(path, "generation-two", false));
  IntegrityFileInfo second;
  FABRIC_CHECK_OK(read_integrity_file(path, &second));
  FABRIC_CHECK_EQ(second.previous_digest, first.payload_digest);
}

FABRIC_TEST(persistence, record_log_round_trip_and_damaged_tail) {
  test::TempDir directory("persistence");
  const std::string path = directory.sub("records.fxr");
  FABRIC_CHECK_OK(append_record(path, "one"));
  FABRIC_CHECK_OK(append_record(path, "two"));
  FABRIC_CHECK_OK(append_record(path, "three"));
  auto report = read_records(path);
  FABRIC_CHECK_OK(report);
  FABRIC_CHECK_EQ(report.value().records.size(), static_cast<std::size_t>(3));
  FABRIC_CHECK(!report.value().truncated);
  FABRIC_CHECK(!report.value().damaged);

  // A truncated tail stops the replay at the last intact record.
  std::string bytes = read_raw(path);
  write_raw(path, bytes.substr(0, bytes.size() - 3));
  auto truncated = read_records(path);
  FABRIC_CHECK_OK(truncated);
  FABRIC_CHECK_EQ(truncated.value().records.size(), static_cast<std::size_t>(2));
  FABRIC_CHECK(truncated.value().truncated);

  // A corrupted payload is reported as damage, not as data.
  test::TempDir second("persistence");
  const std::string other = second.sub("records.fxr");
  FABRIC_CHECK_OK(append_record(other, "alpha"));
  FABRIC_CHECK_OK(append_record(other, "beta"));
  std::string other_bytes = read_raw(other);
  other_bytes[other_bytes.size() - 1] = static_cast<char>(other_bytes[other_bytes.size() - 1] ^ 0x33);
  write_raw(other, other_bytes);
  auto damaged = read_records(other);
  FABRIC_CHECK_OK(damaged);
  FABRIC_CHECK_EQ(damaged.value().records.size(), static_cast<std::size_t>(1));
  FABRIC_CHECK(damaged.value().damaged);
  FABRIC_CHECK(!damaged.value().diagnostic.empty());
}

FABRIC_TEST(persistence, missing_log_reads_as_empty) {
  test::TempDir directory("persistence");
  auto report = read_records(directory.sub("nothing.fxr"));
  FABRIC_CHECK_OK(report);
  FABRIC_CHECK_EQ(report.value().records.size(), static_cast<std::size_t>(0));
}

FABRIC_TEST(framing, round_trip_and_flags) {
  const FrameLimits limits;
  auto encoded = encode_frame("payload-bytes", static_cast<std::uint8_t>(FrameFlags::Response), limits);
  FABRIC_CHECK_OK(encoded);
  FrameDecoder decoder(limits);
  std::vector<Frame> frames;
  FABRIC_CHECK_OK(decoder.feed(encoded.value(), frames));
  FABRIC_CHECK_EQ(frames.size(), static_cast<std::size_t>(1));
  FABRIC_CHECK_EQ(frames.front().payload, std::string("payload-bytes"));
  FABRIC_CHECK(frames.front().has_flag(FrameFlags::Response));
  FABRIC_CHECK_OK(decoder.finish());
}

FABRIC_TEST(framing, fragmented_stream_is_reassembled) {
  const FrameLimits limits;
  auto first = encode_frame("first-frame", 0, limits);
  auto second = encode_frame("second-frame", 0, limits);
  FABRIC_CHECK_OK(first);
  FABRIC_CHECK_OK(second);
  std::string stream = first.value() + second.value();

  FrameDecoder decoder(limits);
  std::vector<Frame> frames;
  for (char byte : stream) {
    FABRIC_CHECK_OK(decoder.feed(std::string_view(&byte, 1), frames));
  }
  FABRIC_CHECK_EQ(frames.size(), static_cast<std::size_t>(2));
  FABRIC_CHECK_EQ(frames[0].payload, std::string("first-frame"));
  FABRIC_CHECK_EQ(frames[1].payload, std::string("second-frame"));
}

FABRIC_TEST(framing, adversarial_inputs_are_refused) {
  const FrameLimits limits;
  auto encoded = encode_frame("payload", 0, limits);
  FABRIC_CHECK_OK(encoded);
  const std::string good = encoded.value();

  {
    std::string bad_magic = good;
    bad_magic[1] = 'Z';
    FrameDecoder decoder(limits);
    std::vector<Frame> frames;
    FABRIC_CHECK_ERR(decoder.feed(bad_magic, frames), ErrorCode::ProtocolViolation);
  }
  {
    std::string bad_version = good;
    bad_version[4] = static_cast<char>(9);
    FrameDecoder decoder(limits);
    std::vector<Frame> frames;
    FABRIC_CHECK_ERR(decoder.feed(bad_version, frames), ErrorCode::ProtocolViolation);
  }
  {
    std::string bad_header = good;
    bad_header[9] = static_cast<char>(bad_header[9] ^ 0x01);
    FrameDecoder decoder(limits);
    std::vector<Frame> frames;
    FABRIC_CHECK_ERR(decoder.feed(bad_header, frames), ErrorCode::ProtocolViolation);
  }
  {
    std::string bad_payload = good;
    bad_payload[bad_payload.size() - 1] = static_cast<char>(bad_payload.back() ^ 0x01);
    FrameDecoder decoder(limits);
    std::vector<Frame> frames;
    FABRIC_CHECK_ERR(decoder.feed(bad_payload, frames), ErrorCode::IntegrityFailure);
  }
  {
    // A header that declares four gigabytes must be refused before allocation.
    FrameDecoder decoder(limits);
    std::vector<Frame> frames;
    std::string huge = good;
    huge[8] = static_cast<char>(0xFF);
    huge[9] = static_cast<char>(0xFF);
    huge[10] = static_cast<char>(0xFF);
    huge[11] = static_cast<char>(0x7F);
    const std::uint32_t header_crc = crc32(std::string_view(huge.data(), 12));
    huge[12] = static_cast<char>(header_crc & 0xFFU);
    huge[13] = static_cast<char>((header_crc >> 8U) & 0xFFU);
    huge[14] = static_cast<char>((header_crc >> 16U) & 0xFFU);
    huge[15] = static_cast<char>((header_crc >> 24U) & 0xFFU);
    FABRIC_CHECK_ERR(decoder.feed(huge, frames), ErrorCode::BoundsExceeded);
  }
  {
    // A partial frame followed by end of stream is reported, not ignored.
    FrameDecoder decoder(limits);
    std::vector<Frame> frames;
    FABRIC_CHECK_OK(decoder.feed(good.substr(0, 10), frames));
    FABRIC_CHECK_ERR(decoder.finish(), ErrorCode::Malformed);
  }
}

FABRIC_TEST(framing, buffer_and_payload_limits_are_enforced) {
  FrameLimits limits;
  limits.max_payload_bytes = 16;
  FABRIC_CHECK_ERR(encode_frame(std::string(17, 'x'), 0, limits), ErrorCode::BoundsExceeded);
  FABRIC_CHECK_OK(encode_frame(std::string(16, 'x'), 0, limits));

  FrameLimits tiny_buffer;
  tiny_buffer.max_buffered_bytes = 40;
  FrameDecoder decoder(tiny_buffer);
  std::vector<Frame> frames;
  FABRIC_CHECK_ERR(decoder.feed(std::string(64, 'q'), frames), ErrorCode::BoundsExceeded);
  FABRIC_CHECK_EQ(decoder.buffered_bytes(), static_cast<std::size_t>(0));
}

FABRIC_TEST(framing, empty_payload_frames_are_valid) {
  FrameLimits limits;
  auto encoded = encode_frame("", 0, limits);
  FABRIC_CHECK_OK(encoded);
  FrameDecoder decoder(limits);
  std::vector<Frame> frames;
  FABRIC_CHECK_OK(decoder.feed(encoded.value(), frames));
  FABRIC_CHECK_EQ(frames.size(), static_cast<std::size_t>(1));
  FABRIC_CHECK(frames.front().payload.empty());
}

}  // namespace fabric::evolution
