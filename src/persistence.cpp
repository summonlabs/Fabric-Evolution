// Fabric Evolution — integrity-checked durable persistence (implementation).
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric/evolution/persistence.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "fabric/evolution/json.hpp"
#include "fabric/evolution/strong_types.hpp"

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace fabric::evolution {
namespace {

constexpr char kMagic[8] = {'F', 'A', 'B', 'E', 'V', 'O', '1', '\0'};
constexpr std::uint32_t kFormatVersion = 1;
constexpr char kRecordMagic[4] = {'F', 'X', 'R', '1'};
constexpr std::size_t kRecordHeaderBytes = 24;  // magic(4) + version(4) + length(8) + crc(4) + reserved(4)

[[nodiscard]] std::string backup_path(const std::string& path) { return path + ".prev"; }

[[nodiscard]] std::string temporary_path(const std::string& path) {
#ifdef _WIN32
  const unsigned long process_id = GetCurrentProcessId();
#else
  const unsigned long process_id = static_cast<unsigned long>(::getpid());
#endif
  return path + ".tmp-" + std::to_string(process_id);
}

[[nodiscard]] Status flush_and_close(std::FILE* handle, const std::string& path) {
  if (std::fflush(handle) != 0) {
    std::fclose(handle);
    return Status::error(ErrorCode::IoFailure, "failed to flush durable file", Json::object({{"path", Json(path)}}));
  }
#ifdef _WIN32
  if (_commit(_fileno(handle)) != 0) {
    std::fclose(handle);
    return Status::error(ErrorCode::IoFailure, "failed to commit durable file",
                         Json::object({{"path", Json(path)}}));
  }
#else
  if (::fsync(::fileno(handle)) != 0) {
    std::fclose(handle);
    return Status::error(ErrorCode::IoFailure, "failed to fsync durable file",
                         Json::object({{"path", Json(path)}}));
  }
#endif
  if (std::fclose(handle) != 0) {
    return Status::error(ErrorCode::IoFailure, "failed to close durable file", Json::object({{"path", Json(path)}}));
  }
  return Status::success();
}

[[nodiscard]] Status replace_file(const std::string& from, const std::string& to) {
#ifdef _WIN32
  if (MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    return Status::error(ErrorCode::IoFailure, "failed to install durable file",
                         Json::object({{"from", Json(from)}, {"to", Json(to)},
                               {"win32_error", Json(static_cast<std::uint64_t>(GetLastError()))}}));
  }
  return Status::success();
#else
  std::error_code error;
  std::filesystem::rename(from, to, error);
  if (error) {
    return Status::error(ErrorCode::IoFailure, "failed to install durable file",
                         Json::object({{"from", Json(from)}, {"to", Json(to)}, {"error", Json(error.message())}}));
  }
  return Status::success();
#endif
}

[[nodiscard]] Result<std::string> read_binary_file(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return Status::error(ErrorCode::IoFailure, "cannot open file for reading", Json::object({{"path", Json(path)}}));
  }
  std::string contents;
  stream.seekg(0, std::ios::end);
  const std::streamoff size = stream.tellg();
  if (size < 0) {
    return Status::error(ErrorCode::IoFailure, "cannot determine file size", Json::object({{"path", Json(path)}}));
  }
  if (static_cast<std::uint64_t>(size) > static_cast<std::uint64_t>(kMaxIntegrityPayloadBytes) +
                                                 kIntegrityHeaderBytes) {
    return Status::error(ErrorCode::BoundsExceeded, "file exceeds the permitted size",
                         Json::object({{"path", Json(path)}, {"bytes", Json(static_cast<std::uint64_t>(size))}}));
  }
  contents.resize(static_cast<std::size_t>(size));
  stream.seekg(0, std::ios::beg);
  if (!contents.empty()) {
    stream.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!stream) {
      return Status::error(ErrorCode::IoFailure, "failed to read file contents",
                           Json::object({{"path", Json(path)}}));
    }
  }
  return contents;
}

[[nodiscard]] Status verify_integrity_bytes(const std::string& raw, IntegrityFileInfo* info,
                                            std::string& payload_out, const std::string& path) {
  if (raw.size() < kIntegrityHeaderBytes) {
    return Status::error(ErrorCode::IntegrityFailure, "integrity file is shorter than its header",
                         Json::object({{"path", Json(path)}, {"bytes", Json(static_cast<std::uint64_t>(raw.size()))}}));
  }
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(raw.data());
  if (std::memcmp(bytes, kMagic, sizeof(kMagic)) != 0) {
    return Status::error(ErrorCode::IntegrityFailure, "integrity file magic mismatch",
                         Json::object({{"path", Json(path)}}));
  }
  const std::uint32_t format_version = read_u32_le(bytes + 8);
  const std::uint32_t header_bytes = read_u32_le(bytes + 12);
  if (format_version != kFormatVersion) {
    return Status::error(ErrorCode::IntegrityFailure, "unsupported integrity file format version",
                         Json::object({{"path", Json(path)}, {"found", Json(format_version)},
                               {"expected", Json(kFormatVersion)}}));
  }
  if (header_bytes != kIntegrityHeaderBytes) {
    return Status::error(ErrorCode::IntegrityFailure, "integrity file header size is not the declared size",
                         Json::object({{"path", Json(path)}, {"header_bytes", Json(header_bytes)}}));
  }
  const std::uint64_t payload_bytes = read_u64_le(bytes + 16);
  const std::uint32_t payload_crc = read_u32_le(bytes + 24);
  if (payload_bytes > kMaxIntegrityPayloadBytes) {
    return Status::error(ErrorCode::BoundsExceeded, "integrity file payload exceeds the permitted size");
  }
  const auto payload_end = checked_add(static_cast<std::size_t>(payload_bytes), kIntegrityHeaderBytes);
  if (!payload_end.has_value() || *payload_end != raw.size()) {
    return Status::error(ErrorCode::IntegrityFailure,
                         "integrity file length does not match the declared payload size",
                         Json::object({{"path", Json(path)},
                               {"declared", Json(payload_bytes)},
                               {"actual", Json(static_cast<std::uint64_t>(raw.size()))}}));
  }
  Digest::bytes_type digest_bytes{};
  for (std::size_t index = 0; index < digest_bytes.size(); ++index) {
    digest_bytes[index] = bytes[32 + index];
  }
  Digest::bytes_type previous_bytes{};
  for (std::size_t index = 0; index < previous_bytes.size(); ++index) {
    previous_bytes[index] = bytes[64 + index];
  }

  const std::string payload = raw.substr(kIntegrityHeaderBytes, static_cast<std::size_t>(payload_bytes));
  const std::uint32_t computed_crc =
      crc32(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size()));
  if (computed_crc != payload_crc) {
    return Status::error(ErrorCode::IntegrityFailure, "integrity file payload CRC mismatch",
                         Json::object({{"path", Json(path)}, {"recorded", Json(payload_crc)},
                               {"computed", Json(computed_crc)}}));
  }
  const Digest computed_digest = sha256(payload);
  if (computed_digest != Digest(digest_bytes)) {
    return Status::error(ErrorCode::IntegrityFailure, "integrity file payload digest mismatch",
                         Json::object({{"path", Json(path)}, {"recorded", Json(Digest(digest_bytes).to_hex())},
                               {"computed", Json(computed_digest.to_hex())}}));
  }
  if (info != nullptr) {
    info->format_version = format_version;
    info->payload_bytes = payload_bytes;
    info->payload_crc32 = payload_crc;
    info->payload_digest = Digest(digest_bytes);
    info->previous_digest = Digest(previous_bytes);
  }
  payload_out = payload;
  return Status::success();
}

[[nodiscard]] std::string encode_integrity_file(std::string_view payload, const Digest& previous_digest) {
  std::string out;
  out.resize(kIntegrityHeaderBytes, '\0');
  std::memcpy(out.data(), kMagic, sizeof(kMagic));
  const auto* payload_bytes = reinterpret_cast<const std::uint8_t*>(payload.data());
  std::string header_tail;
  append_u32_le(header_tail, kFormatVersion);
  append_u32_le(header_tail, static_cast<std::uint32_t>(kIntegrityHeaderBytes));
  append_u64_le(header_tail, static_cast<std::uint64_t>(payload.size()));
  append_u32_le(header_tail, crc32(std::span<const std::uint8_t>(payload_bytes, payload.size())));
  append_u32_le(header_tail, 0U);
  std::memcpy(out.data() + 8, header_tail.data(), header_tail.size());
  const Digest digest = sha256(payload);
  std::memcpy(out.data() + 32, digest.bytes().data(), digest.bytes().size());
  std::memcpy(out.data() + 64, previous_digest.bytes().data(), previous_digest.bytes().size());
  out.append(payload);
  return out;
}

}  // namespace

FileLock::~FileLock() { release(); }

FileLock::FileLock(FileLock&& other) noexcept
#ifdef _WIN32
    : handle_(other.handle_)
#else
    : descriptor_(other.descriptor_)
#endif
{
#ifdef _WIN32
  other.handle_ = nullptr;
#else
  other.descriptor_ = -1;
#endif
}

FileLock& FileLock::operator=(FileLock&& other) noexcept {
  if (this != &other) {
    release();
#ifdef _WIN32
    handle_ = other.handle_;
    other.handle_ = nullptr;
#else
    descriptor_ = other.descriptor_;
    other.descriptor_ = -1;
#endif
  }
  return *this;
}

bool FileLock::held() const noexcept {
#ifdef _WIN32
  return handle_ != nullptr;
#else
  return descriptor_ >= 0;
#endif
}

void FileLock::release() noexcept {
#ifdef _WIN32
  if (handle_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
  }
#else
  if (descriptor_ >= 0) {
    ::flock(descriptor_, LOCK_UN);
    ::close(descriptor_);
    descriptor_ = -1;
  }
#endif
}

Result<FileLock> FileLock::acquire(const std::string& path) {
#ifdef _WIN32
  const HANDLE handle = ::CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                      OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD error = ::GetLastError();
    return Status::error(ErrorCode::AlreadyExists,
                         error == ERROR_SHARING_VIOLATION
                             ? "state directory is already owned by a running process"
                             : "cannot acquire the state directory lock",
                         Json::object({{"path", Json(path)},
                                       {"win32_error", Json(static_cast<std::uint64_t>(error))}}));
  }
  FileLock lock;
  lock.handle_ = handle;
  return lock;
#else
  const int descriptor = ::open(path.c_str(), O_CREAT | O_RDWR, 0600);
  if (descriptor < 0) {
    return Status::error(ErrorCode::IoFailure, "cannot open the state directory lock",
                         Json::object({{"path", Json(path)}}));
  }
  if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
    ::close(descriptor);
    return Status::error(ErrorCode::AlreadyExists,
                         "state directory is already owned by a running process",
                         Json::object({{"path", Json(path)}}));
  }
  FileLock lock;
  lock.descriptor_ = descriptor;
  return lock;
#endif
}

Status ensure_directory(const std::string& path) {
  if (path.empty()) {
    return invalid_argument("directory path must not be empty");
  }
  std::error_code error;
  if (std::filesystem::exists(path, error)) {
    if (std::filesystem::is_directory(path, error)) {
      return Status::success();
    }
    return Status::error(ErrorCode::IoFailure, "path exists but is not a directory",
                         Json::object({{"path", Json(path)}}));
  }
  std::filesystem::create_directories(path, error);
  if (error) {
    return Status::error(ErrorCode::IoFailure, "cannot create directory",
                         Json::object({{"path", Json(path)}, {"error", Json(error.message())}}));
  }
  return Status::success();
}

bool path_exists(const std::string& path) {
  std::error_code error;
  return std::filesystem::exists(path, error);
}

Status remove_file(const std::string& path) {
  std::error_code error;
  std::filesystem::remove(path, error);
  if (error) {
    return Status::error(ErrorCode::IoFailure, "cannot remove file",
                         Json::object({{"path", Json(path)}, {"error", Json(error.message())}}));
  }
  return Status::success();
}

Result<std::string> read_integrity_file(const std::string& path, IntegrityFileInfo* info) {
  auto raw = read_binary_file(path);
  if (!raw.ok()) {
    return raw.status();
  }
  std::string payload;
  const Status status = verify_integrity_bytes(raw.value(), info, payload, path);
  if (!status.ok()) {
    return status;
  }
  return payload;
}

Status write_integrity_file(const std::string& path, std::string_view payload, bool keep_backup) {
  if (payload.size() > kMaxIntegrityPayloadBytes) {
    return Status::error(ErrorCode::BoundsExceeded, "integrity file payload exceeds the permitted size",
                         Json::object({{"path", Json(path)}, {"bytes", Json(static_cast<std::uint64_t>(payload.size()))}}));
  }
  Digest previous_digest;
  if (path_exists(path)) {
    IntegrityFileInfo previous_info;
    auto previous = read_integrity_file(path, &previous_info);
    if (previous.ok()) {
      previous_digest = previous_info.payload_digest;
      if (keep_backup) {
        std::error_code error;
        std::filesystem::copy_file(path, backup_path(path),
                                   std::filesystem::copy_options::overwrite_existing, error);
        if (error) {
          return Status::error(ErrorCode::IoFailure, "cannot refresh the integrity file backup",
                               Json::object({{"path", Json(path)}, {"error", Json(error.message())}}));
        }
      }
    }
  }

  const std::string encoded = encode_integrity_file(payload, previous_digest);
  const std::string temporary = temporary_path(path);
  std::FILE* handle = std::fopen(temporary.c_str(), "wb");
  if (handle == nullptr) {
    return Status::error(ErrorCode::IoFailure, "cannot open temporary file for writing",
                         Json::object({{"path", Json(temporary)}}));
  }
  if (!encoded.empty() &&
      std::fwrite(encoded.data(), 1, encoded.size(), handle) != encoded.size()) {
    std::fclose(handle);
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return Status::error(ErrorCode::IoFailure, "failed to write durable file",
                         Json::object({{"path", Json(temporary)}}));
  }
  const Status flushed = flush_and_close(handle, temporary);
  if (!flushed.ok()) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return flushed;
  }
  const Status replaced = replace_file(temporary, path);
  if (!replaced.ok()) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return replaced;
  }
  return Status::success();
}

Status write_bytes_atomic(const std::string& path, std::string_view bytes) {
  const std::string temporary = temporary_path(path);
  std::FILE* handle = std::fopen(temporary.c_str(), "wb");
  if (handle == nullptr) {
    return Status::error(ErrorCode::IoFailure, "cannot open temporary file for writing",
                         Json::object({{"path", Json(temporary)}}));
  }
  if (!bytes.empty() && std::fwrite(bytes.data(), 1, bytes.size(), handle) != bytes.size()) {
    std::fclose(handle);
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return Status::error(ErrorCode::IoFailure, "failed to write temporary file",
                         Json::object({{"path", Json(temporary)}}));
  }
  const Status flushed = flush_and_close(handle, temporary);
  if (!flushed.ok()) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return flushed;
  }
  const Status replaced = replace_file(temporary, path);
  if (!replaced.ok()) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return replaced;
  }
  return Status::success();
}

Result<RecoveryOutcome> read_integrity_file_with_recovery(const std::string& path, bool allow_backup) {
  RecoveryOutcome outcome;
  auto primary = read_integrity_file(path, &outcome.info);
  if (primary.ok()) {
    outcome.payload = primary.take();
    return outcome;
  }
  if (!allow_backup) {
    return primary.status();
  }
  const std::string backup = backup_path(path);
  if (!path_exists(backup)) {
    return Status::error(primary.status().code(),
                         primary.status().message() + " (no backup copy is available)",
                         primary.status().detail());
  }
  auto fallback = read_integrity_file(backup, &outcome.info);
  if (!fallback.ok()) {
    return Status::error(fallback.status().code(),
                         std::string("primary record is damaged and the backup is unusable: ") +
                             fallback.status().message(),
                         Json::object({{"primary", Json(primary.status().to_string())},
                               {"backup", Json(fallback.status().to_string())}}));
  }
  outcome.payload = fallback.take();
  outcome.from_backup = true;
  return outcome;
}

Status append_record(const std::string& path, std::string_view payload) {
  if (payload.size() > kMaxRecordBytes) {
    return Status::error(ErrorCode::BoundsExceeded, "log record exceeds the permitted size",
                         Json::object({{"bytes", Json(static_cast<std::uint64_t>(payload.size()))},
                               {"limit", Json(static_cast<std::uint64_t>(kMaxRecordBytes))}}));
  }
  std::FILE* handle = std::fopen(path.c_str(), "ab");
  if (handle == nullptr) {
    return Status::error(ErrorCode::IoFailure, "cannot open log for appending", Json::object({{"path", Json(path)}}));
  }
  std::string header;
  header.append(kRecordMagic, sizeof(kRecordMagic));
  append_u32_le(header, kFormatVersion);
  append_u64_le(header, static_cast<std::uint64_t>(payload.size()));
  append_u32_le(header, crc32(payload));
  append_u32_le(header, 0U);
  if (header.size() != kRecordHeaderBytes) {
    std::fclose(handle);
    return Status::error(ErrorCode::Internal, "log record header size is inconsistent");
  }
  const bool header_ok = std::fwrite(header.data(), 1, header.size(), handle) == header.size();
  const bool payload_ok =
      payload.empty() || std::fwrite(payload.data(), 1, payload.size(), handle) == payload.size();
  if (!header_ok || !payload_ok) {
    std::fclose(handle);
    return Status::error(ErrorCode::IoFailure, "failed to append log record", Json::object({{"path", Json(path)}}));
  }
  return flush_and_close(handle, path);
}

Result<LogReadReport> read_records(const std::string& path) {
  LogReadReport report;
  if (!path_exists(path)) {
    return report;
  }
  auto raw = read_binary_file(path);
  if (!raw.ok()) {
    return raw.status();
  }
  const std::string& contents = raw.value();
  std::size_t offset = 0;
  while (offset < contents.size()) {
    if (report.total_records >= kMaxLogRecords) {
      return Status::error(ErrorCode::ResourceExhausted, "log contains more records than the reader permits",
                           Json::object({{"path", Json(path)}, {"limit", Json(static_cast<std::uint64_t>(kMaxLogRecords))}}));
    }
    if (contents.size() - offset < kRecordHeaderBytes) {
      report.truncated = true;
      report.damaged_tail_bytes = static_cast<std::uint64_t>(contents.size() - offset);
      report.diagnostic = "log ends inside a record header";
      return report;
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(contents.data() + offset);
    if (std::memcmp(bytes, kRecordMagic, sizeof(kRecordMagic)) != 0) {
      report.damaged = true;
      report.damaged_tail_bytes = static_cast<std::uint64_t>(contents.size() - offset);
      report.diagnostic = "log record magic mismatch";
      return report;
    }
    const std::uint32_t version = read_u32_le(bytes + 4);
    if (version != kFormatVersion) {
      report.damaged = true;
      report.damaged_tail_bytes = static_cast<std::uint64_t>(contents.size() - offset);
      report.diagnostic = "log record format version mismatch";
      return report;
    }
    const std::uint64_t length = read_u64_le(bytes + 8);
    const std::uint32_t recorded_crc = read_u32_le(bytes + 16);
    if (length > kMaxRecordBytes) {
      report.damaged = true;
      report.damaged_tail_bytes = static_cast<std::uint64_t>(contents.size() - offset);
      report.diagnostic = "log record declares an implausible length";
      return report;
    }
    const auto end = checked_add(offset, checked_add(kRecordHeaderBytes, static_cast<std::size_t>(length))
                                             .value_or(SIZE_MAX));
    if (!end.has_value() || *end > contents.size()) {
      report.truncated = true;
      report.damaged_tail_bytes = static_cast<std::uint64_t>(contents.size() - offset);
      report.diagnostic = "log ends inside a record payload";
      return report;
    }
    const std::string payload = contents.substr(offset + kRecordHeaderBytes, static_cast<std::size_t>(length));
    if (crc32(payload) != recorded_crc) {
      report.damaged = true;
      report.damaged_tail_bytes = static_cast<std::uint64_t>(contents.size() - offset);
      report.diagnostic = "log record CRC mismatch";
      return report;
    }
    report.records.push_back(payload);
    ++report.total_records;
    offset = *end;
  }
  return report;
}

}  // namespace fabric::evolution
