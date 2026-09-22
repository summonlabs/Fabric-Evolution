# Fabric Evolution — architecture

This document states what the runtime is, why each mechanism exists, and where the boundaries are.
It describes the implemented system, not an intended one.

## 1. Systems boundary

Fabric Evolution is a **Fabric OS component**. Its subject is the control plane itself: the
processes, the protocol they speak, the durable state they replicate, and the authority they hold.

It sits between two neighbours and depends on neither for its correctness:

* **Fabric Compatibility Registry** — supplies a typed *compatibility decision*. Fabric Evolution
  binds the decision into a sealed manifest by content digest and refuses to proceed when the
  evidence is absent, unsupported, stale, or has moved on in a way the configured policy does not
  permit. Compatibility is never re-derived here.
* **Rollout Fabric / Upgrade Manager** — may supply the mechanics of placing a new artifact. Fabric
  Evolution does not require it: the manifest declares endpoints and versions, and the controller
  talks to the components directly.

What Fabric Evolution owns is **evolution correctness**: who is authoritative, what the shard's
state may contain, which protocol may be used, which boundaries have been crossed, and what happens
if the evolution must stop.

### Not implemented, and not claimed

No consensus, no quorum, no leader election, no distributed transaction, no byzantine-fault
tolerance. Authority is decided by one controller with durable state; the controllers' decisions
are enforced by leases with a bounded lifetime plus explicit fencing, and by the components
themselves self-fencing when a lease lapses. Replication is one-way (predecessor → successor) and
is used for handoff catch-up, not for availability.

## 2. Identity model

Every identity is a distinct C++ type; a value of one kind cannot be passed where another is
expected, and no identity decays to a bare string or integer.

| Identity | Meaning |
| --- | --- |
| \`ComponentId\` | Validated textual component name; the durable key for a component |
| \`ShardId\` | Validated textual shard name |
| \`IncarnationId\` | \`(component, 128-bit boot uuid, monotonic boot counter)\` — one process lifetime |
| \`SoftwareVersion\` | \`major.minor.patch[+build]\`; build metadata participates in ordering |
| \`ProtocolVersion\` | \`major.minor\`; major decides compatibility, minor is the negotiation axis |
| \`SchemaVersion\` | Monotonic version of the replicated state document |
| \`EpochNumber\` | A fenced, durable era of a shard |
| \`Generation\` | Increments on every authority change; the stale-claim axis |
| \`LeaseId\`, \`AuthorityToken\` | 128-bit opaque lease identity and authority token |
| \`HandoffId\`, \`FenceId\`, \`SnapshotId\` | Identities of the evolution's own artefacts |
| \`LogSequenceNumber\` | Position in the replicated log |
| \`ManifestId\`, \`CampaignId\`, \`MigrationStepId\` | Evolution declarations |

Zero and nil are reserved as "unset". Decoders reject a missing or malformed identity rather than
defaulting it.

### Fresh-incarnation fencing

A component takes **exclusive ownership of its state directory** for the process lifetime
(\`CreateFile\` with no sharing on Windows, \`flock\` elsewhere). A restart therefore reads the durable
boot counter, increments it, and starts with a strictly greater value; a second process cannot share
the directory. That single-writer property is what makes "the component has presented a greater boot
counter" a proof that the previous process is gone, and it is the only inference the controller uses
to revoke a dead incarnation's lease.

An incarnation's authority is bound to its identity. After a restart the component holds no usable
authority: a persisted lease is loaded as evidence and marked unconfirmed, and it authorises nothing
until the controller re-establishes it for the new incarnation.

## 3. Authority model

The authority registry is the single decision point for "who may mutate shard S right now".

* **Modes.** \`None\`, \`ReadOnlyShared\`, \`Mutating\`. Reads are permitted for both shared and mutating
  holders; mutation only for a live, attested mutating lease.
* **Invariant I1.** At most one lease per shard is in mode \`Mutating\` and state \`Active\`.
* **Invariant I2.** Generation is strictly monotonic; only a change of *mutating* holder bumps it, so
  a read-only shared lease never invalidates the incumbent writer's claims.
* **Invariant I3.** A mutating lease is granted to a new holder only once the previous holder's lease
  is \`Fenced\`, \`Expired\` or \`Revoked\`. A lease in state \`Fencing\` keeps the slot busy — and it can
  never be renewed, because that would resurrect an incarnation the controller has ordered to stop.
* **Lease states.** \`Active\`, \`Fencing\` (fence issued, not yet acknowledged), \`Fenced\`,
  \`Expired\`, \`Revoked\`. Expiry is evaluated on every decision, so a lapsed lease stops authorising
  immediately, whether or not housekeeping has run.
* **Safe shared phase.** Multiple holders may hold \`ReadOnlyShared\` leases concurrently. This is the
  only phase in which more than one component is authoritative, and it carries no mutation authority
  by construction. The reference handoff does not require it, but the model permits it explicitly.
* **Fencing.** A fence bumps the generation, marks the target's lease \`Fencing\`, and is complete only
  when the target acknowledges, when its lease expires, or when the target is provably replaced.
  Until then the slot stays busy and no successor can be granted authority.

Every authority decision is appended to the durable epoch journal before it is acknowledged.

## 4. Manifest admission

A manifest declares: source and target component specs (identity, software, protocol, schema,
features, artifact digest, endpoint), the allowed mixed-version window, the compatibility evidence,
the ordered state-migration steps, the protocol negotiation path, the irreversible boundaries, and
the rollback/forward-recovery strategy.

Admission validates, among other rules:

* the embedded compatibility decision covers exactly the manifest's versions and protocols;
* the decision's digest verifies, and the decision is not \`Unsupported\`;
* migration steps form a contiguous chain from the source schema to the target schema, are
  deterministic, and never move the schema backwards;
* every step marked \`irreversible_boundary\` is also declared in the rollback strategy;
* \`ForwardRecoveryOnly\` is permitted only when the path actually crosses an irreversible boundary,
  and \`RollbackToPredecessor\` is refused when it does;
* the protocol path accepts both endpoints and contains no major-incompatible version;
* every required feature is allowed, supported by both participants, and **certified by the
  compatibility registry**;
* the window has positive, bounded duration and operation limits.

A sealed manifest carries a SHA-256 digest over its canonical encoding; the controller re-verifies it
before every campaign and refuses a manifest whose digest does not match.

## 5. Handoff lifecycle

\`\`\`
NotStarted → Prepared → SnapshotSynchronized → CaughtUp → ReadinessVerified
           → PredecessorFenced → AuthorityTransferred → SuccessorVerified
           → PredecessorRetired
\`\`\`

| Phase | Work |
| --- | --- |
| \`Prepared\` | Predecessor confirms it holds live authority; successor verifies its own version, protocol and schema against the manifest target and records the source binding |
| \`SnapshotSynchronized\` | Predecessor produces an integrity-checked snapshot; successor verifies the digest and the source schema before installing |
| \`CaughtUp\` | Controller pumps incremental records from the predecessor into the successor, then applies the declared migration steps |
| \`ReadinessVerified\` | Successor attests readiness: schema at target, log position reached, lifecycle serving |
| \`PredecessorFenced\` | Registry fences the predecessor; the predecessor acknowledges |
| \`AuthorityTransferred\` | A final catch-up against the now-stable predecessor, then a new generation and token are granted to the successor |
| \`SuccessorVerified\` | Controller proves authority uniqueness: the successor attests the new authority, the predecessor's own pre-fence credentials are presented and **must be refused**, and the successor accepts a write |
| \`PredecessorRetired\` | Predecessor is retired; it refuses every mutation permanently |

Properties of the machine:

* Transitions are strictly one step at a time and validated; anything else is refused.
* Every transition is durable before it is acknowledged; a controller killed between phases resumes
  from the persisted phase.
* Replaying a completed phase is a no-op, which is what makes duplicate handoff frames harmless.
* The authority-transfer window (\`PredecessorFenced\`, \`AuthorityTransferred\`) is the one place where
  pause is **deferred** rather than honoured, because pausing there could leave a shard ownerless.
* A successor that dies before authority moves causes the handoff to rewind (to the start, or to the
  snapshot phase when the predecessor is already fenced) for the new incarnation, with a bumped
  attempt and an explicit journal record.
* A fenced predecessor is still a valid **replication source** — it may serve snapshots and records,
  it simply may not mutate. That is what lets a restarted successor rejoin without unfencing anyone.

## 6. State, schema and migration

Replicated state is a versioned document: a schema version plus a sorted key/value payload with a
content digest. Migration steps are declarative — a built-in function name plus string parameters —
so a manifest can never smuggle in arbitrary code. The built-ins are \`identity\`, \`add_field\`,
\`rename_field\`, \`remove_field\`, \`wrap_value\`, \`unwrap_value\` and \`lowercase_field\`; each has an
exact inverse or is declared non-reversible, and steps may mark a field \`optional\` to make an absent
field a no-op instead of a failure.

* **Determinism.** A step that is not declared deterministic is refused at resolution time.
* **Evidence binding.** A step may declare an evidence digest computed over the step declaration
  (excluding the digest field itself) and the digest of the state it is applied to. Installing
  different state under the same step fails with \`integrity_failure\`.
* **Irreversible boundaries.** Once crossed, \`migrate_backward\` refuses with \`irreversible_boundary\`
  naming the step. The campaign records the crossing and switches its recovery strategy.
* **Journaling.** Every applied step is recorded with the state digest before and after, so a
  restart cannot re-apply or silently skip a step.

## 7. Protocol negotiation and feature gating

Negotiation is deterministic and total: the same participant hellos and the same manifest path always
produce the same effective protocol version and the same enabled feature set.

1. Protocol majors must match, or the negotiation is refused.
2. The effective version is the lower minor.
3. The effective version must be on the manifest's accepted path.
4. Every participant's schema must be inside the manifest's declared schema envelope.
5. The enabled feature set is the intersection of all participants, restricted to the manifest's
   allowed set; a required feature missing from any participant refuses the negotiation.
6. A required participant that does not present a hello refuses the negotiation.

Multi-party negotiation intersects across all participants, so a feature is never enabled unless
every required participant offers it, the manifest allows it, and the registry certified it.

## 8. Persistence and recovery

Two on-disk primitives:

* **Integrity file** — fixed header, CRC-32 and SHA-256 over the payload, plus the previous
  generation's digest. Writes go to a temporary file, are flushed and committed, and are then
  installed by an atomic replace. A previous copy is kept alongside.
* **Record log** — append-only, length-prefixed, CRC-checked records with a magic and a version.

Recovery is conservative by construction:

* A record that fails verification is never treated as valid.
* A damaged primary can be recovered from the backup copy **only** when the caller asks, and the
  recovery is reported (\`from_backup\`), never silent.
* A log replay stops at the first damaged or truncated record and reports how many records were
  recoverable and what was wrong.
* The durable state file is the **commit point**: a record in the log beyond the committed log
  position was never acknowledged, so it is dropped and the log is rewritten, rather than being
  replayed as accepted.
* A damaged identity file or state file stops the boot. The component refuses to start with a guessed
  identity rather than risk becoming a second authority.

## 9. Restart-safe controller

The controller persists its manifest, campaign, handoff, authority registry, epoch counter and the
predecessor claim it fenced. On restart:

1. Durable state is integrity-checked and loaded; a backup recovery is reported.
2. **Every restored lease is marked unconfirmed.** Persisted dynamic evidence never becomes current
   merely because it can be deserialised.
3. Endpoints are re-derived from the manifest, so the restarted controller knows where the components
   are.
4. Reconciliation queries both components and:
   * re-attests or renews the incumbent's lease when the component confirms the exact token;
   * revokes a lease held by an incarnation the component has provably replaced;
   * acknowledges a fence whose target reports itself fenced;
   * re-binds or restarts the handoff for participant incarnations that changed;
   * advances the phase when a component's report proves a phase completed before the crash.
5. Only then does the campaign continue.

## 10. Concurrency discipline

* The controller and each node own exactly one mutex each. Public entry points acquire it once and
  delegate to \`*_locked\` implementations that never acquire it. This is enforced by construction:
  the mutex is not recursive, so any accidental re-entry fails immediately rather than deadlocking.
* Server handlers run on worker threads and are never invoked while a server lock is held.
* Lock ordering: the acceptor never holds the queue lock while taking the socket-registry lock.
* Shutdown stops admission first, closes the listener, wakes blocked readers by shutting down their
  sockets, and only then joins worker threads. No lock is held across a join.
* An operation that is cancelled or deferred never later publishes success: the phase is persisted
  only after the work it describes has completed.

## 11. Resource bounds

Every externally derived size is bounded and checked: frame payload and buffer sizes, log record
sizes, snapshot and state document sizes, key and value lengths, field counts, batch sizes, lease
TIDs, handoff attempt counts, restart counts, campaign and handoff history lengths, epoch journal
length (compacted by rewriting the retained tail), record-log length (compacted while keeping the
catch-up window), fault-rule counts, connection queue depth and worker threads. Size arithmetic on
externally derived values uses checked helpers that reject overflow.
