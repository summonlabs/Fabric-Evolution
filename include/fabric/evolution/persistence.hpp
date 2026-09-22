// Fabric Evolution — integrity-checked durable persistence.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Two on-disk primitives are used by the runtime:
//
//   * IntegrityFile — a single record with a fixed header, a CRC-32 over the
//     payload and a SHA-256 content digest. Writes are atomic: the payload is
//     written to a temporary file, flushed, and renamed over the target, so a
//     crash can never leave a half-written record in place.
//   * RecordLog — an append-only sequence of length-prefixed, CRC-checked
//     records. Reading stops at the first damaged record and reports how many
//     records were recoverable, rather than trusting a truncated tail.
//
// Recovery is conservative by construction: a record that fails verification is
// never silently treated as valid, and a record that was recovered from a backup
// copy is always reported as such.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "fabric/evolution/digest.hpp"
#include "fabric/evolution/status.hpp"

namespace fabric::evolution {

inline constexpr std::size_t kIntegrityHeaderBytes = 128;
inline constexpr std::size_t kMaxIntegrityPayloadBytes = 64U * 1024U * 1024U;
inline constexpr std::size_t kMaxRecordBytes = 8U * 1024U * 1024U;
inline constexpr std::size_t kMaxLogRecords = 1'000'000;

struct IntegrityFileInfo {
  std::uint32_t format_version = 0;
  std::uint64_t payload_bytes = 0;
  std::uint32_t payload_crc32 = 0;
  Digest payload_digest;
  Digest previous_digest;
};

[[nodiscard]] Status ensure_directory(const std::string& path);
[[nodiscard]] bool path_exists(const std::string& path);
[[nodiscard]] Status remove_file(const std::string& path);

// Reads and verifies an integrity file. Never throws; failures are reported as
// IntegrityFailure (damaged) or IoFailure (unreadable).
[[nodiscard]] Result<std::string> read_integrity_file(const std::string& path, IntegrityFileInfo* info);
// Writes atomically. When keep_backup is set, the previous contents are moved to
// path + ".prev" before the replacement is installed.
[[nodiscard]] Status write_integrity_file(const std::string& path, std::string_view payload,
                                          bool keep_backup);

// Atomic replacement of an arbitrary byte file (used to compact record logs).
[[nodiscard]] Status write_bytes_atomic(const std::string& path, std::string_view bytes);

// An exclusive, process-lifetime lock on a state directory. It exists so that
// the runtime can treat a component that presents a strictly greater durable
// boot counter as proof that the previous process is gone: without single-writer
// semantics a second live process could share the same state directory, and the
// fencing inference below would be unsound.
class FileLock {
 public:
  FileLock() = default;
  ~FileLock();
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;
  FileLock(FileLock&& other) noexcept;
  FileLock& operator=(FileLock&& other) noexcept;

  [[nodiscard]] static Result<FileLock> acquire(const std::string& path);
  [[nodiscard]] bool held() const noexcept;
  void release() noexcept;

 private:
#ifdef _WIN32
  void* handle_ = nullptr;
#else
  int descriptor_ = -1;
#endif
};

struct RecoveryOutcome {
  std::string payload;
  IntegrityFileInfo info;
  bool from_backup = false;
};

// Reads the primary file; if it is damaged and allow_backup is set, retries the
// backup copy and reports from_backup = true. Silent fallback never happens.
[[nodiscard]] Result<RecoveryOutcome> read_integrity_file_with_recovery(const std::string& path,
                                                                       bool allow_backup);

// Appends one length-prefixed, CRC-checked record.
[[nodiscard]] Status append_record(const std::string& path, std::string_view payload);

struct LogReadReport {
  std::vector<std::string> records;
  std::uint64_t total_records = 0;
  std::uint64_t damaged_tail_bytes = 0;
  bool truncated = false;   // the log ended mid-record
  bool damaged = false;     // a record failed verification
  std::string diagnostic;
};

// Reads every intact record from the front of the log and reports the state of
// the tail.
[[nodiscard]] Result<LogReadReport> read_records(const std::string& path);

}  // namespace fabric::evolution
