// Fabric Evolution — error codes and status rendering (implementation).
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric/evolution/status.hpp"

namespace fabric::evolution {

std::string_view to_string(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::Ok:
      return "ok";
    case ErrorCode::InvalidArgument:
      return "invalid_argument";
    case ErrorCode::Malformed:
      return "malformed";
    case ErrorCode::BoundsExceeded:
      return "bounds_exceeded";
    case ErrorCode::NotFound:
      return "not_found";
    case ErrorCode::AlreadyExists:
      return "already_exists";
    case ErrorCode::Conflict:
      return "conflict";
    case ErrorCode::StaleEpoch:
      return "stale_epoch";
    case ErrorCode::StaleGeneration:
      return "stale_generation";
    case ErrorCode::StaleIncarnation:
      return "stale_incarnation";
    case ErrorCode::StaleAuthority:
      return "stale_authority";
    case ErrorCode::NotAuthoritative:
      return "not_authoritative";
    case ErrorCode::AuthorityConflict:
      return "authority_conflict";
    case ErrorCode::CompatibilityInsufficient:
      return "compatibility_insufficient";
    case ErrorCode::IrreversibleBoundary:
      return "irreversible_boundary";
    case ErrorCode::UnsupportedProtocol:
      return "unsupported_protocol";
    case ErrorCode::FeatureNotNegotiated:
      return "feature_not_negotiated";
    case ErrorCode::MigrationFailure:
      return "migration_failure";
    case ErrorCode::IntegrityFailure:
      return "integrity_failure";
    case ErrorCode::IoFailure:
      return "io_failure";
    case ErrorCode::Cancelled:
      return "cancelled";
    case ErrorCode::ShuttingDown:
      return "shutting_down";
    case ErrorCode::DeadlineExceeded:
      return "deadline_exceeded";
    case ErrorCode::ProtocolViolation:
      return "protocol_violation";
    case ErrorCode::DuplicateFrame:
      return "duplicate_frame";
    case ErrorCode::NotReady:
      return "not_ready";
    case ErrorCode::IllegalTransition:
      return "illegal_transition";
    case ErrorCode::ResourceExhausted:
      return "resource_exhausted";
    case ErrorCode::Unsupported:
      return "unsupported";
    case ErrorCode::Internal:
      return "internal";
  }
  return "unknown";
}

bool is_retryable(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::NotReady:
    case ErrorCode::Conflict:
    case ErrorCode::DeadlineExceeded:
    case ErrorCode::ResourceExhausted:
    case ErrorCode::IoFailure:
      return true;
    default:
      return false;
  }
}

std::string Status::to_string() const {
  if (ok()) {
    return "ok";
  }
  std::string out(::fabric::evolution::to_string(code_));
  if (!message_.empty()) {
    out += ": ";
    out += message_;
  }
  return out;
}

Json Status::to_json() const {
  Json out = Json::object();
  out.set("code", Json(std::string(::fabric::evolution::to_string(code_))));
  out.set("message", Json(message_));
  if (!detail_.is_null()) {
    out.set("detail", detail_);
  }
  return out;
}

}  // namespace fabric::evolution
