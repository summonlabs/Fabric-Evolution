// Fabric Evolution — versioned state and deterministic state migration.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Replicated state is carried as a versioned document: a schema version plus a
// sorted key/value payload with a content digest. Migration steps are declarative
// (a named built-in function plus parameters), deterministic, integrity checked,
// and journaled. Crossing a step that is marked irreversible makes downgrade
// impossible, and the migrator says so explicitly instead of corrupting state.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fabric/evolution/clock.hpp"
#include "fabric/evolution/digest.hpp"
#include "fabric/evolution/ids.hpp"
#include "fabric/evolution/manifest.hpp"

namespace fabric::evolution {

inline constexpr std::size_t kMaxStateKeyBytes = 256;
inline constexpr std::size_t kMaxStateValueBytes = 64U * 1024U;
inline constexpr std::size_t kMaxStateFields = 100000;

class StateDocument {
 public:
  StateDocument() = default;
  explicit StateDocument(SchemaVersion schema) : schema_(schema) {}

  [[nodiscard]] SchemaVersion schema() const noexcept { return schema_; }
  [[nodiscard]] Status set_schema(SchemaVersion schema);

  [[nodiscard]] Status put(std::string key, std::string value);
  [[nodiscard]] Status remove(std::string_view key);
  [[nodiscard]] std::optional<std::string> get(std::string_view key) const;
  [[nodiscard]] bool contains(std::string_view key) const;

  using FieldMap = std::map<std::string, std::string, std::less<>>;

  [[nodiscard]] const FieldMap& fields() const noexcept { return fields_; }
  [[nodiscard]] std::size_t size() const noexcept { return fields_.size(); }
  [[nodiscard]] bool empty() const noexcept { return fields_.empty(); }

  // Digest over the canonical encoding of the schema version and every field.
  [[nodiscard]] Digest digest() const;
  [[nodiscard]] Json to_json() const;
  [[nodiscard]] static Result<StateDocument> from_json(const Json& value);

  friend bool operator==(const StateDocument&, const StateDocument&) noexcept = default;

 private:
  SchemaVersion schema_;
  FieldMap fields_;
};

enum class MigrationOpKind : std::uint8_t {
  Identity = 0,
  AddField = 1,
  RenameField = 2,
  RemoveField = 3,
  WrapValue = 4,
  UnwrapValue = 5,
  LowercaseField = 6,
};

[[nodiscard]] std::string_view to_string(MigrationOpKind kind) noexcept;

struct ResolvedMigration {
  MigrationStepSpec spec;
  MigrationOpKind kind = MigrationOpKind::Identity;
  std::map<std::string, std::string> args;
};

// Resolves a declared step into a built-in transformation. Unknown function
// names and missing or oversized parameters are refused.
[[nodiscard]] Result<ResolvedMigration> resolve_migration_step(const MigrationStepSpec& spec);

// Evidence binding: the digest a manifest declares for a step is a digest over
// the step declaration and the digest of the state it is applied to. A mismatch
// means the successor would accept state that is not bound to the source
// generation, so the migration is refused.
[[nodiscard]] Digest migration_evidence_digest(const MigrationStepSpec& spec,
                                               const Digest& state_digest_before);

struct MigrationStepRecord {
  MigrationStepId step;
  SchemaVersion from;
  SchemaVersion to;
  Digest state_digest_before;
  Digest state_digest_after;
  Digest evidence_digest;
  bool irreversible_boundary = false;
};

struct MigrationOutcome {
  SchemaVersion from;
  SchemaVersion to;
  std::vector<MigrationStepRecord> steps;
  Digest state_digest_before;
  Digest state_digest_after;
  bool crossed_irreversible_boundary = false;
  std::vector<MigrationStepId> irreversible_boundaries_crossed;
};

class StateMigrator {
 public:
  // Applies every step in order. The document is left unchanged on failure.
  [[nodiscard]] Result<MigrationOutcome> migrate_forward(StateDocument& document,
                                                         const std::vector<MigrationStepSpec>& path) const;

  // Reverse migration. Refuses as soon as a step is not reversible, naming the
  // boundary that makes the downgrade unsafe. The path argument is the forward
  // path; the migrator reverses it.
  [[nodiscard]] Result<MigrationOutcome> migrate_backward(StateDocument& document,
                                                          const std::vector<MigrationStepSpec>& path) const;
};

// Journal of applied migrations, bounded and serializable.
struct MigrationJournalRecord {
  ShardId shard;
  ComponentId component;
  MigrationStepId step;
  SchemaVersion from;
  SchemaVersion to;
  Digest state_digest_after;
  TimestampMs applied_at_ms = 0;
  bool irreversible_boundary = false;
};

class MigrationJournal {
 public:
  explicit MigrationJournal(std::size_t capacity = 4096) : capacity_(capacity == 0 ? 1 : capacity) {}

  [[nodiscard]] bool has_applied(const ShardId& shard, const ComponentId& component,
                                 const MigrationStepId& step, SchemaVersion from,
                                 SchemaVersion to) const;
  Status record(MigrationJournalRecord entry);
  [[nodiscard]] const std::vector<MigrationJournalRecord>& records() const noexcept { return records_; }
  [[nodiscard]] std::vector<MigrationJournalRecord> for_component(const ShardId& shard,
                                                                  const ComponentId& component) const;
  [[nodiscard]] std::optional<SchemaVersion> highest_schema(const ShardId& shard,
                                                            const ComponentId& component) const;
  [[nodiscard]] bool irreversible_crossed(const ShardId& shard, const ComponentId& component) const;

  [[nodiscard]] Json to_json() const;
  Status load(const Json& value);
  void clear();

 private:
  std::vector<MigrationJournalRecord> records_;
  std::size_t capacity_;
};

}  // namespace fabric::evolution
