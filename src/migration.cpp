// Fabric Evolution — versioned state and deterministic state migration.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric/evolution/migration.hpp"

#include <algorithm>

namespace fabric::evolution {
namespace {

constexpr std::size_t kMaxMigrationArgumentBytes = 4096;

[[nodiscard]] Result<std::string> require_arg(const MigrationStepSpec& spec,
                                              const std::map<std::string, std::string>& args,
                                              const char* name) {
  const auto it = args.find(name);
  if (it == args.end() || it->second.empty()) {
    return Status::error(ErrorCode::Unsupported,
                         "migration step " + spec.id.str() + " requires argument '" + name + "'");
  }
  return it->second;
}

[[nodiscard]] bool argument_is_true(const std::map<std::string, std::string>& args, const char* name) {
  const auto it = args.find(name);
  return it != args.end() && (it->second == "true" || it->second == "1");
}

[[nodiscard]] Result<std::map<std::string, std::string>> read_args(const MigrationStepSpec& spec) {
  std::map<std::string, std::string> args;
  if (!spec.parameters.is_object()) {
    if (!spec.parameters.is_null()) {
      return Status::error(ErrorCode::Unsupported,
                           "migration step " + spec.id.str() + " parameters must be an object");
    }
    return args;
  }
  for (const auto& entry : *spec.parameters.try_object()) {
    const std::string* value = entry.second.try_string();
    if (value == nullptr) {
      return Status::error(ErrorCode::Unsupported, "migration step " + spec.id.str() +
                                                       " parameter '" + entry.first +
                                                       "' must be a string");
    }
    if (entry.first.size() > kMaxStateKeyBytes || value->size() > kMaxMigrationArgumentBytes) {
      return Status::error(ErrorCode::BoundsExceeded,
                           "migration step " + spec.id.str() + " parameter '" + entry.first +
                               "' exceeds the permitted size");
    }
    args.emplace(entry.first, *value);
  }
  return args;
}


// A field that is absent is a migration failure unless the step explicitly
// declares the field optional. Making this explicit keeps "the state did not
// contain what the manifest promised" a detectable condition rather than a
// silent no-op.
[[nodiscard]] bool field_is_optional(const ResolvedMigration& resolved) {
  return argument_is_true(resolved.args, "optional");
}

[[nodiscard]] Status apply_forward(const ResolvedMigration& resolved, StateDocument& document) {
  switch (resolved.kind) {
    case MigrationOpKind::Identity:
      break;
    case MigrationOpKind::AddField: {
      const auto key = require_arg(resolved.spec, resolved.args, "key");
      if (!key.ok()) {
        return key.status();
      }
      const auto value = require_arg(resolved.spec, resolved.args, "value");
      if (!value.ok()) {
        return value.status();
      }
      const Status put = document.put(key.value(), value.value());
      if (!put.ok()) {
        return put;
      }
      break;
    }
    case MigrationOpKind::RenameField: {
      const auto from = require_arg(resolved.spec, resolved.args, "from");
      const auto to = require_arg(resolved.spec, resolved.args, "to");
      if (!from.ok()) {
        return from.status();
      }
      if (!to.ok()) {
        return to.status();
      }
      const auto value = document.get(from.value());
      if (!value.has_value()) {
        if (field_is_optional(resolved)) {
          break;
        }
        return Status::error(ErrorCode::MigrationFailure,
                             "migration step " + resolved.spec.id.str() +
                                 " cannot rename a field that is absent: " + from.value());
      }
      if (document.contains(to.value())) {
        return Status::error(ErrorCode::MigrationFailure,
                             "migration step " + resolved.spec.id.str() +
                                 " would overwrite an existing field: " + to.value());
      }
      const Status put = document.put(to.value(), *value);
      if (!put.ok()) {
        return put;
      }
      const Status removed = document.remove(from.value());
      if (!removed.ok()) {
        return removed;
      }
      break;
    }
    case MigrationOpKind::RemoveField: {
      const auto key = require_arg(resolved.spec, resolved.args, "key");
      if (!key.ok()) {
        return key.status();
      }
      if (!document.contains(key.value())) {
        if (field_is_optional(resolved)) {
          break;
        }
        return Status::error(ErrorCode::MigrationFailure,
                             "migration step " + resolved.spec.id.str() +
                                 " cannot remove a field that is absent: " + key.value());
      }
      const Status removed = document.remove(key.value());
      if (!removed.ok()) {
        return removed;
      }
      break;
    }
    case MigrationOpKind::WrapValue: {
      const auto key = require_arg(resolved.spec, resolved.args, "key");
      const auto prefix = require_arg(resolved.spec, resolved.args, "prefix");
      const auto suffix = require_arg(resolved.spec, resolved.args, "suffix");
      if (!key.ok()) {
        return key.status();
      }
      if (!prefix.ok()) {
        return prefix.status();
      }
      if (!suffix.ok()) {
        return suffix.status();
      }
      const auto value = document.get(key.value());
      if (!value.has_value()) {
        if (field_is_optional(resolved)) {
          break;
        }
        return Status::error(ErrorCode::MigrationFailure,
                             "migration step " + resolved.spec.id.str() +
                                 " cannot wrap a field that is absent: " + key.value());
      }
      std::string wrapped = prefix.value();
      wrapped += *value;
      wrapped += suffix.value();
      const Status put = document.put(key.value(), std::move(wrapped));
      if (!put.ok()) {
        return put;
      }
      break;
    }
    case MigrationOpKind::UnwrapValue: {
      const auto key = require_arg(resolved.spec, resolved.args, "key");
      const auto prefix = require_arg(resolved.spec, resolved.args, "prefix");
      const auto suffix = require_arg(resolved.spec, resolved.args, "suffix");
      if (!key.ok()) {
        return key.status();
      }
      if (!prefix.ok()) {
        return prefix.status();
      }
      if (!suffix.ok()) {
        return suffix.status();
      }
      const auto value = document.get(key.value());
      if (!value.has_value()) {
        return Status::error(ErrorCode::MigrationFailure,
                             "migration step " + resolved.spec.id.str() +
                                 " cannot unwrap a field that is absent: " + key.value());
      }
      const std::string& text = *value;
      if (text.size() < prefix.value().size() + suffix.value().size() ||
          text.compare(0, prefix.value().size(), prefix.value()) != 0 ||
          text.compare(text.size() - suffix.value().size(), suffix.value().size(), suffix.value()) != 0) {
        return Status::error(ErrorCode::MigrationFailure,
                             "migration step " + resolved.spec.id.str() +
                                 " cannot unwrap a field whose value lacks the expected framing: " +
                                 key.value());
      }
      const std::string unwrapped =
          text.substr(prefix.value().size(), text.size() - prefix.value().size() - suffix.value().size());
      const Status put = document.put(key.value(), unwrapped);
      if (!put.ok()) {
        return put;
      }
      break;
    }
    case MigrationOpKind::LowercaseField: {
      const auto key = require_arg(resolved.spec, resolved.args, "key");
      if (!key.ok()) {
        return key.status();
      }
      const auto value = document.get(key.value());
      if (!value.has_value()) {
        return Status::error(ErrorCode::MigrationFailure,
                             "migration step " + resolved.spec.id.str() +
                                 " cannot lowercase a field that is absent: " + key.value());
      }
      std::string lowered = *value;
      std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
        return static_cast<char>((c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c);
      });
      const Status put = document.put(key.value(), std::move(lowered));
      if (!put.ok()) {
        return put;
      }
      break;
    }
  }
  return Status::success();
}

}  // namespace

Status StateDocument::set_schema(SchemaVersion schema) {
  if (!schema.is_valid()) {
    return invalid_argument("schema version must be positive");
  }
  schema_ = schema;
  return Status::success();
}

Status StateDocument::put(std::string key, std::string value) {
  if (key.empty() || key.size() > kMaxStateKeyBytes) {
    return Status::error(ErrorCode::BoundsExceeded, "state key size is outside the permitted range",
                         Json::object({{"key_bytes", Json(static_cast<std::uint64_t>(key.size()))},
                               {"limit", Json(static_cast<std::uint64_t>(kMaxStateKeyBytes))}}));
  }
  if (value.size() > kMaxStateValueBytes) {
    return Status::error(ErrorCode::BoundsExceeded, "state value size exceeds the permitted limit",
                         Json::object({{"value_bytes", Json(static_cast<std::uint64_t>(value.size()))},
                               {"limit", Json(static_cast<std::uint64_t>(kMaxStateValueBytes))}}));
  }
  const auto existing = fields_.find(key);
  if (existing == fields_.end() && fields_.size() >= kMaxStateFields) {
    return Status::error(ErrorCode::ResourceExhausted, "state document field limit reached",
                         Json::object({{"limit", Json(static_cast<std::uint64_t>(kMaxStateFields))}}));
  }
  fields_.insert_or_assign(std::move(key), std::move(value));
  return Status::success();
}

Status StateDocument::remove(std::string_view key) {
  const auto it = fields_.find(key);
  if (it == fields_.end()) {
    return Status::error(ErrorCode::NotFound, "state field not found: " + std::string(key));
  }
  fields_.erase(it);
  return Status::success();
}

std::optional<std::string> StateDocument::get(std::string_view key) const {
  const auto it = fields_.find(key);
  if (it == fields_.end()) {
    return std::nullopt;
  }
  return it->second;
}

bool StateDocument::contains(std::string_view key) const { return fields_.find(key) != fields_.end(); }

Digest StateDocument::digest() const { return sha256(to_json().dump()); }

Json StateDocument::to_json() const {
  Json out = Json::object();
  out.set("schema", Json(schema_.value()));
  Json payload = Json::object();
  for (const auto& entry : fields_) {
    payload.set(entry.first, Json(entry.second));
  }
  out.set("fields", std::move(payload));
  return out;
}

Result<StateDocument> StateDocument::from_json(const Json& value) {
  if (!value.is_object()) {
    return malformed("state document must be an object");
  }
  const auto schema = json_u64(value, "schema");
  if (!schema.has_value() || *schema == 0 || *schema > UINT32_MAX) {
    return malformed("state document schema version is missing or out of range");
  }
  StateDocument document(SchemaVersion::from_value(static_cast<std::uint32_t>(*schema)));
  const Json* fields = value.find("fields");
  if (fields == nullptr || !fields->is_object()) {
    return malformed("state document must contain a fields object");
  }
  const Json::Object& object = *fields->try_object();
  if (object.size() > kMaxStateFields) {
    return Status::error(ErrorCode::BoundsExceeded, "state document exceeds the field limit");
  }
  for (const auto& entry : object) {
    const std::string* text = entry.second.try_string();
    if (text == nullptr) {
      return malformed("state document field '" + entry.first + "' must be a string");
    }
    const Status put = document.put(entry.first, *text);
    if (!put.ok()) {
      return put;
    }
  }
  return document;
}

std::string_view to_string(MigrationOpKind kind) noexcept {
  switch (kind) {
    case MigrationOpKind::Identity:
      return "identity";
    case MigrationOpKind::AddField:
      return "add_field";
    case MigrationOpKind::RenameField:
      return "rename_field";
    case MigrationOpKind::RemoveField:
      return "remove_field";
    case MigrationOpKind::WrapValue:
      return "wrap_value";
    case MigrationOpKind::UnwrapValue:
      return "unwrap_value";
    case MigrationOpKind::LowercaseField:
      return "lowercase_field";
  }
  return "unknown";
}

Result<ResolvedMigration> resolve_migration_step(const MigrationStepSpec& spec) {
  ResolvedMigration resolved;
  resolved.spec = spec;
  auto args = read_args(spec);
  if (!args.ok()) {
    return args.status();
  }
  resolved.args = args.take();

  if (spec.function == "identity") {
    resolved.kind = MigrationOpKind::Identity;
  } else if (spec.function == "add_field") {
    resolved.kind = MigrationOpKind::AddField;
  } else if (spec.function == "rename_field") {
    resolved.kind = MigrationOpKind::RenameField;
  } else if (spec.function == "remove_field") {
    resolved.kind = MigrationOpKind::RemoveField;
  } else if (spec.function == "wrap_value") {
    resolved.kind = MigrationOpKind::WrapValue;
  } else if (spec.function == "unwrap_value") {
    resolved.kind = MigrationOpKind::UnwrapValue;
  } else if (spec.function == "lowercase_field") {
    resolved.kind = MigrationOpKind::LowercaseField;
  } else {
    return Status::error(ErrorCode::Unsupported,
                         "unknown migration function: " + spec.function,
                         Json::object({{"step", Json(spec.id.str())}}));
  }
  if (!spec.deterministic) {
    return Status::error(ErrorCode::Unsupported,
                         "migration step " + spec.id.str() + " is declared non-deterministic");
  }
  return resolved;
}

Digest migration_evidence_digest(const MigrationStepSpec& spec, const Digest& state_digest_before) {
  // The evidence digest is computed over the step declaration *without* the
  // evidence field itself, so sealing a step does not change what it commits to.
  MigrationStepSpec declarative = spec;
  declarative.evidence_digest = Digest{};
  Json out = Json::object();
  out.set("step", ::fabric::evolution::to_json(declarative));
  out.set("state_digest_before", Json(state_digest_before.to_hex()));
  return sha256(out.dump());
}

Result<MigrationOutcome> StateMigrator::migrate_forward(StateDocument& document,
                                                        const std::vector<MigrationStepSpec>& path) const {
  MigrationOutcome outcome;
  outcome.from = document.schema();
  outcome.state_digest_before = document.digest();

  StateDocument working = document;
  SchemaVersion current = working.schema();
  for (const MigrationStepSpec& spec : path) {
    if (spec.from != current) {
      return Status::error(ErrorCode::MigrationFailure,
                           "migration path is not contiguous at step " + spec.id.str(),
                           Json::object({{"expected_from", Json(current.value())},
                                 {"step_from", Json(spec.from.value())}}));
    }
    auto resolved = resolve_migration_step(spec);
    if (!resolved.ok()) {
      return resolved.status();
    }
    const Digest before = working.digest();
    if (!spec.evidence_digest.is_zero()) {
      const Digest expected = migration_evidence_digest(spec, before);
      if (expected != spec.evidence_digest) {
        return Status::error(
            ErrorCode::IntegrityFailure,
            "migration step " + spec.id.str() + " evidence does not bind to the state it was applied to",
            Json::object({{"declared", Json(spec.evidence_digest.to_hex())},
                  {"computed", Json(expected.to_hex())},
                  {"state_digest_before", Json(before.to_hex())}}));
      }
    }
    const Status applied = apply_forward(resolved.value(), working);
    if (!applied.ok()) {
      return applied;
    }
    const Status schema_set = working.set_schema(spec.to);
    if (!schema_set.ok()) {
      return schema_set;
    }
    MigrationStepRecord record;
    record.step = spec.id;
    record.from = spec.from;
    record.to = spec.to;
    record.state_digest_before = before;
    record.state_digest_after = working.digest();
    record.evidence_digest = spec.evidence_digest;
    record.irreversible_boundary = spec.irreversible_boundary;
    outcome.steps.push_back(record);
    if (spec.irreversible_boundary) {
      outcome.crossed_irreversible_boundary = true;
      outcome.irreversible_boundaries_crossed.push_back(spec.id);
    }
    current = spec.to;
  }

  outcome.to = current;
  outcome.state_digest_after = working.digest();
  document = std::move(working);
  return outcome;
}

Result<MigrationOutcome> StateMigrator::migrate_backward(StateDocument& document,
                                                         const std::vector<MigrationStepSpec>& path) const {
  MigrationOutcome outcome;
  outcome.from = document.schema();
  outcome.state_digest_before = document.digest();

  StateDocument working = document;
  SchemaVersion current = working.schema();
  for (auto it = path.rbegin(); it != path.rend(); ++it) {
    const MigrationStepSpec& spec = *it;
    if (spec.to != current) {
      return Status::error(ErrorCode::MigrationFailure,
                           "reverse migration path is not contiguous at step " + spec.id.str(),
                           Json::object({{"expected_to", Json(current.value())},
                                 {"step_to", Json(spec.to.value())}}));
    }
    auto resolved = resolve_migration_step(spec);
    if (!resolved.ok()) {
      return resolved.status();
    }
    if (spec.irreversible_boundary || !spec.reversible) {
      return Status::error(
          ErrorCode::IrreversibleBoundary,
          "cannot downgrade across irreversible migration boundary " + spec.id.str(),
          Json::object({{"step", Json(spec.id.str())},
                {"from_schema", Json(spec.from.value())},
                {"to_schema", Json(spec.to.value())},
                {"function", Json(spec.function)}}));
    }

    const Digest before = working.digest();
    // Reverse the built-in transformation. Each reversible function has an
    // exact inverse; a function without one can never be marked reversible.
    ResolvedMigration reverse;
    reverse.spec = spec;
    switch (resolved.value().kind) {
      case MigrationOpKind::Identity:
        reverse.kind = MigrationOpKind::Identity;
        reverse.args = resolved.value().args;
        break;
      case MigrationOpKind::AddField:
        reverse.kind = MigrationOpKind::RemoveField;
        reverse.args["key"] = resolved.value().args.at("key");
        break;
      case MigrationOpKind::RenameField:
        reverse.kind = MigrationOpKind::RenameField;
        reverse.args["from"] = resolved.value().args.at("to");
        reverse.args["to"] = resolved.value().args.at("from");
        break;
      case MigrationOpKind::WrapValue:
        reverse.kind = MigrationOpKind::UnwrapValue;
        reverse.args = resolved.value().args;
        break;
      default:
        return Status::error(ErrorCode::IrreversibleBoundary,
                             "migration step " + spec.id.str() + " has no reverse transformation",
                             Json::object({{"function", Json(spec.function)}}));
    }

    const Status applied = apply_forward(reverse, working);
    if (!applied.ok()) {
      return applied;
    }
    const Status schema_set = working.set_schema(spec.from);
    if (!schema_set.ok()) {
      return schema_set;
    }
    MigrationStepRecord record;
    record.step = spec.id;
    record.from = spec.to;
    record.to = spec.from;
    record.state_digest_before = before;
    record.state_digest_after = working.digest();
    record.irreversible_boundary = false;
    outcome.steps.push_back(record);
    current = spec.from;
  }

  outcome.to = current;
  outcome.state_digest_after = working.digest();
  document = std::move(working);
  return outcome;
}

bool MigrationJournal::has_applied(const ShardId& shard, const ComponentId& component,
                                   const MigrationStepId& step, SchemaVersion from,
                                   SchemaVersion to) const {
  for (const MigrationJournalRecord& record : records_) {
    if (record.shard == shard && record.component == component && record.step == step &&
        record.from == from && record.to == to) {
      return true;
    }
  }
  return false;
}

Status MigrationJournal::record(MigrationJournalRecord entry) {
  if (!entry.shard.is_valid() || !entry.component.is_valid() || !entry.step.is_valid()) {
    return invalid_argument("migration journal entry identities must be valid");
  }
  if (!entry.from.is_valid() || !entry.to.is_valid()) {
    return invalid_argument("migration journal entry schema versions must be valid");
  }
  records_.push_back(std::move(entry));
  while (records_.size() > capacity_) {
    records_.erase(records_.begin());
  }
  return Status::success();
}

std::vector<MigrationJournalRecord> MigrationJournal::for_component(const ShardId& shard,
                                                                    const ComponentId& component) const {
  std::vector<MigrationJournalRecord> out;
  for (const MigrationJournalRecord& record : records_) {
    if (record.shard == shard && record.component == component) {
      out.push_back(record);
    }
  }
  return out;
}

std::optional<SchemaVersion> MigrationJournal::highest_schema(const ShardId& shard,
                                                              const ComponentId& component) const {
  std::optional<SchemaVersion> highest;
  for (const MigrationJournalRecord& record : records_) {
    if (record.shard != shard || record.component != component) {
      continue;
    }
    if (!highest.has_value() || record.to > *highest) {
      highest = record.to;
    }
  }
  return highest;
}

bool MigrationJournal::irreversible_crossed(const ShardId& shard, const ComponentId& component) const {
  for (const MigrationJournalRecord& record : records_) {
    if (record.shard == shard && record.component == component && record.irreversible_boundary) {
      return true;
    }
  }
  return false;
}

Json MigrationJournal::to_json() const {
  Json out = Json::object();
  Json entries = Json::array();
  for (const MigrationJournalRecord& record : records_) {
    Json entry = Json::object();
    entry.set("shard", Json(record.shard.str()));
    entry.set("component", Json(record.component.str()));
    entry.set("step", Json(record.step.str()));
    entry.set("from", Json(record.from.value()));
    entry.set("to", Json(record.to.value()));
    entry.set("state_digest_after", Json(record.state_digest_after.to_hex()));
    entry.set("applied_at_ms", Json(record.applied_at_ms));
    entry.set("irreversible_boundary", Json(record.irreversible_boundary));
    entries.push_back(std::move(entry));
  }
  out.set("records", std::move(entries));
  return out;
}

Status MigrationJournal::load(const Json& value) {
  if (!value.is_object()) {
    return malformed("migration journal must be an object");
  }
  const Json* records = value.find("records");
  if (records == nullptr || !records->is_array()) {
    return malformed("migration journal must contain a records array");
  }
  std::vector<MigrationJournalRecord> loaded;
  for (std::size_t index = 0; index < records->size(); ++index) {
    const Json& entry = records->at(index);
    if (!entry.is_object()) {
      return malformed("migration journal entry must be an object");
    }
    MigrationJournalRecord record;
    const auto shard = json_string(entry, "shard");
    const auto component = json_string(entry, "component");
    const auto step = json_string(entry, "step");
    if (!shard.has_value() || !component.has_value() || !step.has_value()) {
      return malformed("migration journal entry is missing an identity");
    }
    auto parsed_shard = ShardId::parse(*shard);
    auto parsed_component = ComponentId::parse(*component);
    auto parsed_step = MigrationStepId::parse(*step);
    if (!parsed_shard.has_value() || !parsed_component.has_value() || !parsed_step.has_value()) {
      return malformed("migration journal entry identity is invalid");
    }
    record.shard = *parsed_shard;
    record.component = *parsed_component;
    record.step = *parsed_step;
    const auto from = json_u64(entry, "from");
    const auto to = json_u64(entry, "to");
    if (!from.has_value() || !to.has_value() || *from == 0 || *to == 0 || *from > UINT32_MAX ||
        *to > UINT32_MAX) {
      return malformed("migration journal entry schema versions are missing or out of range");
    }
    record.from = SchemaVersion::from_value(static_cast<std::uint32_t>(*from));
    record.to = SchemaVersion::from_value(static_cast<std::uint32_t>(*to));
    const auto digest = json_string(entry, "state_digest_after");
    if (digest.has_value()) {
      auto parsed_digest = Digest::from_hex(*digest);
      if (!parsed_digest.has_value()) {
        return malformed("migration journal entry digest is not a sha256 hex digest");
      }
      record.state_digest_after = *parsed_digest;
    }
    record.applied_at_ms = json_u64_or(entry, "applied_at_ms", 0);
    record.irreversible_boundary = json_bool_or(entry, "irreversible_boundary", false);
    loaded.push_back(std::move(record));
  }
  records_ = std::move(loaded);
  while (records_.size() > capacity_) {
    records_.erase(records_.begin());
  }
  return Status::success();
}

void MigrationJournal::clear() { records_.clear(); }

}  // namespace fabric::evolution
