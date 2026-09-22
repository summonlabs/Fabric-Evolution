# Fabric Evolution

**Fabric Evolution** is a standalone, vendor-neutral runtime for the controlled evolution of a
distributed control plane while the fabric stays in service. It owns the correctness of an
evolution — not just the mechanics of moving binaries — and it is a Summon Software Labs
Fabric OS component.

It is a C++20 library, three shipped executables, and a test suite that proves the distributed
claims with real independent OS processes over real sockets.

---

## What this repository owns

Fabric Evolution owns the controlled evolution of the control plane itself:

* **Authority handoff** between a predecessor and a successor incarnation of a control-plane
  component, without a global control-plane stop.
* **Mixed-version epochs** — two versions of a component running concurrently, bounded in both
  time and operation count, with feature gating so no feature is enabled unless every required
  participant supports it.
* **State and schema migration** with versioned schemas, deterministic declared steps, integrity
  checks, and an explicit refusal to downgrade across a declared irreversible boundary.
* **Protocol transition** through deterministic negotiation.
* **Component replacement** and **fresh-incarnation fencing** across process restarts.
* **Rollback and forward recovery** — rollback while it is safe, forward recovery once it is not.

### Systems boundary

| Concern | Owner |
| --- | --- |
| *Is this source/target version pair allowed at all?* | **Fabric Compatibility Registry** — consumed, never re-derived here |
| *What are the mechanics of moving an artifact?* | **Rollout Fabric / Upgrade Manager** — may be used, not required |
| *Is this particular evolution correct and safe, and who is authoritative right now?* | **Fabric Evolution (this repository)** |

Fabric Evolution consumes a typed compatibility decision, binds it to a sealed manifest by digest,
and refuses to evolve when the evidence is missing, unsupported, stale, or no longer matches what
the registry says. It does not compute compatibility itself and it does not pretend to.

### Explicitly not implemented

There is **no consensus protocol, no quorum, no leader election and no distributed transaction**
in this repository, and nothing here claims otherwise. Authority is granted by a single controller
and enforced by leases with a bounded lifetime plus explicit fencing. Replication is one-way, from
an authoritative predecessor to a successor, with records bound to the source incarnation, epoch,
generation and schema. Section *Real, synthetic and unsupported* below states the exact proof
boundary.

---

## Architecture at a glance

```
                    ┌──────────────────────────────┐
                    │     fabric-evolution (CLI)   │
                    └───────────────┬──────────────┘
                                    │ admin RPC over framed TCP
                    ┌───────────────▼──────────────┐
                    │  fabric-evolution-controller │   durable state, authority
                    │  campaign + handoff + epoch  │   registry, epoch journal
                    └───────┬──────────────┬───────┘
        node RPC + snapshot │              │ node RPC + catch-up
                    ┌───────▼──────┐  ┌────▼─────────┐
                    │ node-a (v1)  │  │ node-b (v2)  │   control-plane components
                    │ predecessor  │  │ successor    │   under evolution
                    └──────────────┘  └──────────────┘
```

* `fabric-evolution` — operator CLI: `plan`, `preflight`, `start`, `advance`, `pause`,
  `resume`, `abort`, `reconcile`, `status`, `handoff`, `epoch`, `authority`,
  `explain --topic …`.
* `fabric-evolution-controller` — the coordinator. Integrity-checked durable state, restart-safe
  phase progression, reconciliation against component-reported epochs and incarnations.
* `fabric-evolution-node` — a reference replicated/sharded control-plane component with durable
  state, a schema-versioned key/value document, an append-only record log, authority-lease
  enforcement and one-way replication.

Full detail: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

---

## The handoff lifecycle

```
NotStarted → Prepared → SnapshotSynchronized → CaughtUp → ReadinessVerified
           → PredecessorFenced → AuthorityTransferred → SuccessorVerified
           → PredecessorRetired
```

Each transition is durable before it is acknowledged, each is idempotent when replayed, and the
controller must be able to explain, from component reports alone, where a handoff had got to when
it was killed. A controller that dies mid-handoff resumes from the persisted phase; a successor
that dies before authority moves causes the handoff to restart for its new incarnation; a
predecessor that is *provably* replaced (a strictly greater durable boot counter) has its lease
revoked and, if a fence was outstanding, that fence is recorded as acknowledged.

### Invariants the runtime enforces

1. **At most one mutating authority per shard.** Read-only shared leases are the only phase in
   which more than one component is authoritative, and they cannot mutate by construction.
2. **Generation is strictly monotonic.** Every authority change bumps it, so a claim minted by a
   previous holder can never be replayed into the current one.
3. **A mutating lease is only granted once the previous holder is fenced, expired, or revoked.**
   A predecessor that has merely been *asked* to fence keeps the mutation slot busy.
4. **No stale predecessor can act after authoritative fencing.** The controller actively proves it
   during `advance_verify` by presenting the predecessor's own pre-fence credentials and requiring
   a refusal.
5. **Migrated state is bound to its source generation and migration version.** A successor accepts
   replicated records only when they carry exactly the incarnation, epoch, generation, token and
   schema it was prepared for.
6. **A feature is enabled only when every required participant supports it**, the manifest allows
   it, and the compatibility registry has certified it.
7. **Irreversible boundaries are declared before they are crossed.** A manifest that marks a step
   irreversible must also declare that boundary in its rollback strategy, or admission fails.
8. **No global shutdown is required.** The reference evolution path moves authority between live
   processes and keeps serving reads and writes throughout.

---

## Build

Requirements: CMake 3.20+, a C++20 compiler (GCC 11+, Clang 14+, MSVC 19.30+), and nothing else —
the runtime has **no third-party dependencies**.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Options: `FABRIC_EVOLUTION_BUILD_TESTS`, `FABRIC_EVOLUTION_BUILD_TOOLS`,
`FABRIC_EVOLUTION_BUILD_EXAMPLES`, `FABRIC_EVOLUTION_BUILD_BENCHMARKS`,
`FABRIC_EVOLUTION_WARNINGS_AS_ERRORS` (default `ON`), `FABRIC_EVOLUTION_ENABLE_ASAN`
(default `OFF`).

First-party code builds warning-free under `/W4 /WX` (MSVC) and
`-Wall -Wextra -Wpedantic -Wshadow -Wold-style-cast -Werror` (GCC/Clang).

### Install and consume

```sh
cmake --install build --prefix /opt/fabric-evolution
```

```cmake
find_package(FabricEvolution 1.0 REQUIRED CONFIG)
target_link_libraries(my_runtime PRIVATE FabricEvolution::fabric_evolution)
```

The CTest case `fabric_evolution_package_consumer` performs exactly this against the installed
tree: it configures, builds and runs the independent project in `tests/downstream/`.

---

## Running a real evolution

Start two components and a controller (this is what the end-to-end tests do):

```sh
fabric-evolution-node --component node-a --shard shard-0 --state-dir /var/lib/fx/a \
  --software 1.0.0 --protocol 1.0 --schema 1 \
  --features snapshot_transfer,incremental_catch_up,schema_migration,protocol_negotiation,fenced_stale_rejection,epoch_attestation

fabric-evolution-node --component node-b --shard shard-0 --state-dir /var/lib/fx/b \
  --software 2.0.0 --protocol 1.1 --schema 2 \
  --features snapshot_transfer,incremental_catch_up,schema_migration,protocol_negotiation,fenced_stale_rejection,epoch_attestation

fabric-evolution-controller --state-dir /var/lib/fx/controller --compat compatibility.json
```

Each process prints a machine-readable readiness line containing the port it bound. Then:

```sh
fabric-evolution --controller 127.0.0.1:PORT plan --manifest manifest.json
fabric-evolution --controller 127.0.0.1:PORT preflight
fabric-evolution --controller 127.0.0.1:PORT start
fabric-evolution --controller 127.0.0.1:PORT status
fabric-evolution --controller 127.0.0.1:PORT explain --topic window
fabric-evolution --controller 127.0.0.1:PORT authority
fabric-evolution --controller 127.0.0.1:PORT handoff
fabric-evolution --controller 127.0.0.1:PORT epoch
```

Refusals are explained, not merely reported:

```
refused: not_ready: authority is mid-transfer; pausing here could leave a shard
without an owner, so the request is deferred to the next phase boundary
decision: campaign.pause [deferred] policy=fabric-evolution.campaign.v1 epoch=1 generation=3
```

Manifest, compatibility-registry and protocol formats: [docs/OPERATIONS.md](docs/OPERATIONS.md)
and [docs/PROTOCOL.md](docs/PROTOCOL.md). Example documents live in `examples/manifests/`.

---

## Tests

```sh
ctest --test-dir build --output-on-failure
```

| Suite | What it proves |
| --- | --- |
| `test_primitives` | Digests, canonical JSON, versions, feature sets, identity validation |
| `test_authority` | Single-mutating-owner invariant, fencing, expiry, re-attestation after recovery |
| `test_manifest` | Admission rules, irreversible-boundary declaration, digest tamper detection |
| `test_migration` | Forward/backward migration, evidence binding, irreversible downgrade refusal |
| `test_protocol` | Deterministic negotiation, feature gating, multi-party intersection |
| `test_persistence` | Integrity files, backup recovery, damaged record logs, frame codec adversarial input |
| `test_node_process` | Real node processes: durability, fresh-incarnation fencing, corrupt state refusal |
| `test_e2e_evolution` | Complete evolutions with kill/restart of predecessor, successor and controller at every phase |
| `test_e2e_faults` | Corrupt snapshot, lost fence acknowledgement, stale records, migration failure, abort/forward recovery |
| `test_e2e_cli` | The CLI as a real child process against a real controller |
| `test_property` | Seeded randomized authority state machines and migration round trips |
| `test_concurrency` | Concurrent clients, start/stop cycles, status readers during a handoff |

There are no test timeouts anywhere: a hanging test is treated as a defect to diagnose.

## Benchmarks

```sh
./build/benchmarks/fabric-evolution-benchmark --iterations 500 --handoffs 5
```

```
benchmark                             completed    seconds  per second
node.frame_round_trips                      500     0.1481        3376.6   request/response round trips over a real TCP socket
node.durable_writes                         125     1.3396          93.3   authority-checked writes committed to durable state
migration.deterministic_steps               500     0.0032      158087.8   complete forward migration chains applied to a versioned document
manifest.admissions                         125     0.0145        8642.5   manifests validated, sealed and digest-verified
evolution.completed_handoffs                  5     1.3534           3.7   complete control-plane evolutions including process startup
```

Every figure counts *completed* work. Durable writes are slow on purpose: each write is committed
to disk before it is acknowledged, so the cycle time is dominated by the flush.

---

## Real, synthetic and unsupported

**Real** (independent OS processes, real kernel sockets, real kills and restarts):
multi-process handoff, authority fencing, mixed-version negotiation, snapshot transfer,
incremental catch-up, state migration, restart recovery, controller reconciliation, CLI operation,
and the package consumer build.

**Synthetic**: nothing about the distributed claims. Faults such as a corrupted snapshot image or
a lost fence acknowledgement are injected deliberately through the runtime's own bounded fault
surface so that failure paths are exercised rather than argued about; the transport, the
processes and the durable state are real in every one of those runs.

**Unsupported**: RDMA, NVLink, multi-GPU, switch or firmware behaviour, and any hardware-specific
claim. None is made. Fabric Evolution is transport-agnostic above a TCP framing layer and has been
exercised only over loopback TCP on a single host; behaviour across a real network partition,
across hosts, and under adversarial byzantine participants is **not** proven here.

Full statement, including the exact commands that produce each result:
[docs/PROOF.md](docs/PROOF.md).

---

## Documentation

* [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — the systems boundary, identity model, authority
  model, handoff lifecycle, persistence and recovery, concurrency discipline, resource bounds.
* [docs/OPERATIONS.md](docs/OPERATIONS.md) — running components, manifests, compatibility
  evidence, campaign operations, troubleshooting.
* [docs/PROTOCOL.md](docs/PROTOCOL.md) — framed transport, node RPC operations, admin operations.
* [docs/PROOF.md](docs/PROOF.md) — proof obligations, how each is discharged, and what is not
  claimed.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
