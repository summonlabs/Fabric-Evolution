// Fabric Evolution — error codes, status values and result carriers.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "fabric/evolution/json.hpp"

namespace fabric::evolution {

// Error taxonomy. Each code names a distinct refusal or failure reason so that
// a deterministic explanation can be rendered from a status alone.
enum class ErrorCode : std::uint8_t {
  Ok = 0,
  InvalidArgument,
  Malformed,
  BoundsExceeded,
  NotFound,
  AlreadyExists,
  Conflict,
  StaleEpoch,
  StaleGeneration,
  StaleIncarnation,
  StaleAuthority,
  NotAuthoritative,
  AuthorityConflict,
  CompatibilityInsufficient,
  IrreversibleBoundary,
  UnsupportedProtocol,
  FeatureNotNegotiated,
  MigrationFailure,
  IntegrityFailure,
  IoFailure,
  Cancelled,
  ShuttingDown,
  DeadlineExceeded,
  ProtocolViolation,
  DuplicateFrame,
  NotReady,
  IllegalTransition,
  ResourceExhausted,
  Unsupported,
  Internal,
};

[[nodiscard]] std::string_view to_string(ErrorCode code) noexcept;

// A refusal that a peer may legitimately retry after reconciling state.
[[nodiscard]] bool is_retryable(ErrorCode code) noexcept;

class Status {
 public:
  Status() noexcept = default;

  [[nodiscard]] static Status success() noexcept { return Status{}; }
  [[nodiscard]] static Status error(ErrorCode code, std::string message) {
    return Status(code, std::move(message), Json());
  }
  [[nodiscard]] static Status error(ErrorCode code, std::string message, Json detail) {
    return Status(code, std::move(message), std::move(detail));
  }

  [[nodiscard]] bool ok() const noexcept { return code_ == ErrorCode::Ok; }
  [[nodiscard]] bool is_error() const noexcept { return code_ != ErrorCode::Ok; }
  [[nodiscard]] ErrorCode code() const noexcept { return code_; }
  [[nodiscard]] const std::string& message() const noexcept { return message_; }
  [[nodiscard]] const Json& detail() const noexcept { return detail_; }

  [[nodiscard]] std::string to_string() const;
  [[nodiscard]] Json to_json() const;

 private:
  Status(ErrorCode code, std::string message, Json detail)
      : code_(code), message_(std::move(message)), detail_(std::move(detail)) {}

  ErrorCode code_ = ErrorCode::Ok;
  std::string message_;
  Json detail_;
};

// Result<T>: either a value or a refusal.
//
// A Status may be converted into a Result<T> so that failures can propagate with
// a plain "return status;". A *successful* Status carries no value, so that
// conversion is a programming error; it is turned into a loud Internal failure
// here rather than into a result that silently claims to hold a value.
template <class T>
class Result {
 public:
  Result(T value) : storage_(std::move(value)) {}          // NOLINT(google-explicit-constructor)
  Result(Status status) : storage_(make_storage(std::move(status))) {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool ok() const noexcept { return std::holds_alternative<T>(storage_); }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }

  [[nodiscard]] T& value() noexcept { return std::get<T>(storage_); }
  [[nodiscard]] const T& value() const noexcept { return std::get<T>(storage_); }
  [[nodiscard]] T&& take() noexcept { return std::move(std::get<T>(storage_)); }
  [[nodiscard]] const Status& status() const noexcept {
    static const Status kOk;
    const Status* status = std::get_if<Status>(&storage_);
    return status == nullptr ? kOk : *status;
  }

  [[nodiscard]] T value_or(T fallback) const {
    return ok() ? std::get<T>(storage_) : std::move(fallback);
  }

 private:
  [[nodiscard]] static std::variant<T, Status> make_storage(Status status) {
    if (status.ok()) {
      return std::variant<T, Status>(Status::error(
          ErrorCode::Internal,
          "a successful Status cannot be converted into a Result value"));
    }
    return std::variant<T, Status>(std::move(status));
  }

  std::variant<T, Status> storage_;
};

// Helpers used by decoders.
[[nodiscard]] inline Status malformed(std::string message) {
  return Status::error(ErrorCode::Malformed, std::move(message));
}

[[nodiscard]] inline Status invalid_argument(std::string message) {
  return Status::error(ErrorCode::InvalidArgument, std::move(message));
}

[[nodiscard]] inline Status not_found(std::string message) {
  return Status::error(ErrorCode::NotFound, std::move(message));
}

}  // namespace fabric::evolution
