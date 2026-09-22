# Fabric Evolution — proof obligations and evidence

This document states what the repository proves, how, and — just as importantly — what it does
**not** claim.

Reproduce everything with:

\`\`\`sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
\`\`\`

## 1. Classification of proof surfaces

### REAL

Executed with the shipped binaries as independent OS processes, communicating over kernel TCP
sockets on loopback, with real process kills and restarts and real durable files on disk:

* Multi-process handoff across all eight phases.
* Authority uniqueness before, during and after a handoff.
* Continuous service: representative reads and writes issued while the evolution runs.
* Predecessor, successor and controller killed at every handoff phase, then restarted.
* Fresh-incarnation fencing: an old incarnation's credentials are refused after a restart.
* Snapshot transfer, incremental catch-up and state migration carrying real data end to end.
* Mixed-version protocol negotiation and feature gating between two real versions.
* Durable state recovery, integrity checking and single-writer directory locking.
* The CLI as a real child process against a real controller.
* The installed package consumed by an independent downstream CMake project.

### SYNTHETIC (fault injection over real infrastructure)

The faults are injected deliberately; the processes, sockets and durable files they exercise are
real:

* A snapshot whose declared digest does not match its contents.
* A fence that the predecessor applies but whose acknowledgement is lost in transit.
* Replicated records bearing a stale generation, and a stale claim presented to a retired
  predecessor.
* A migration step whose declared evidence does not bind to the state it is applied to.
* A migration step naming a field the state does not contain.
* Compatibility evidence that has moved on, or a verdict that has changed.
* A mixed-version budget exhausted by real writes.
* Aborts before and after an irreversible boundary.

### UNSUPPORTED — not claimed anywhere in this repository

* No consensus, quorum, leader election, distributed transaction or byzantine tolerance.
* No RDMA, NVLink, multi-GPU, switch, firmware or other hardware-specific behaviour.
* No multi-host or cross-datacenter behaviour. All distributed proof is on **loopback TCP within a
  single host**; behaviour across a real network partition is not proven.
* No adversarial or byzantine participant behaviour. The fault model is crash-and-restart plus the
  injected faults listed above.
* No availability or durability guarantee under storage that lies about flushes. Durability relies on
  the platform's flush primitives (\`_commit\` on Windows, \`fsync\` elsewhere) and on atomic replace.

## 2. Obligations and how each is discharged

| Obligation | Mechanism | Where it is proven |
| --- | --- | --- |
| At most one mutating authority per shard | Registry invariant checked on every decision; the mutation slot stays busy while a fence is outstanding | \`test_authority\`, \`test_property\`, every \`e2e\` test asserts \`mutating_authority_count == 1\` |
| No stale predecessor can act after fencing | Generation bump on fence; the controller actively presents the predecessor's pre-fence credentials during \`SuccessorVerified\` and requires a refusal | \`e2e.handoff_completes_with_continuous_service_and_migrated_state\` |
| No evolution step creates two conflicting owners | A new mutating holder is refused while a lease is active or being fenced | \`test_authority.fence_blocks_new_authority_until_acknowledged\`, \`e2e_faults.lost_fence_acknowledgement…\` |
| Safe shared phase is explicitly non-mutating | Concurrent read-only leases; mutation requires the mutating mode | \`test_authority.read_only_shared_phase_is_concurrent_and_non_mutating\` |
| Successor state is bound to the correct source generation and migration version | Source binding carried in every install and apply; migration evidence digest binds a step to the state it is applied to | \`e2e_faults.stale_replicated_records_are_refused\`, \`test_migration.evidence_digest_binds_state_to_the_source_generation\` |
| A feature is never enabled unless every required participant supports it | Multi-party feature intersection plus manifest allow-list plus registry certification | \`test_protocol\`, \`test_manifest\`, \`e2e_faults.feature_missing_from_the_successor_is_refused\` |
| Irreversible boundaries are declared before being crossed | Admission rule; campaign records the crossing; downgrade refused afterwards | \`test_manifest.irreversible_step_must_be_declared_before_start\`, \`test_migration.downgrade_across_an_irreversible_boundary_is_refused\`, \`e2e_faults.abort_after_an_irreversible_boundary_goes_forward\` |
| Global shutdown is not required | Every phase runs against live processes; reads and writes continue throughout | \`e2e.handoff_completes_with_continuous_service_and_migrated_state\` |
| Restart-safe controller with reconciliation | Durable phase, restored leases marked unconfirmed, reconciliation from component reports | \`e2e.controller_kill_and_restart_at_every_phase\`, \`e2e.killed_predecessor_restarts_the_handoff_for_the_new_incarnation\` |
| Fresh-incarnation fencing across process restart | Durable boot counter under exclusive directory ownership; leases bound to an incarnation | \`test_node_process.restart_is_a_new_incarnation_and_old_claims_are_fenced\`, \`test_node_process.damaged_durable_state_stops_the_boot\` |
| Deterministic decisions and explanations | Pure state machines; canonical JSON; identical inputs produce byte-identical documents | \`e2e.explanation_is_deterministic_for_the_same_inputs\`, \`test_protocol.negotiation_is_deterministic\`, \`test_property\` |
| Conservative persistence | CRC plus SHA-256, atomic replace, backup recovery reported not silent, damaged tail stops replay | \`test_persistence\` |
| Bounded resources | Limits on frames, buffers, records, payloads, batches, histories, queues and threads; checked size arithmetic | \`test_persistence\`, \`test_migration.size_bounds_are_enforced\`, \`test_manifest.window_bounds_are_enforced\`, \`test_authority.lease_ttl_bounds_are_checked\` |
| Adversarial input | Truncated, corrupt, mis-declared and malformed documents; malformed JSON; malformed identities | \`test_primitives.json.adversarial_inputs_are_rejected\`, \`test_persistence\`, \`test_manifest.malformed_documents_are_rejected\` |
| Real concurrency behaviour | Eight concurrent clients over real sockets; concurrent status readers during a live handoff; repeated start/stop cycles | \`test_concurrency\` |
| Installable, exportable package | Independent downstream project configured with \`find_package(FabricEvolution CONFIG REQUIRED)\`, built and run | CTest case \`fabric_evolution_package_consumer\` |

## 3. Defects found and fixed during hardening

Each of these was found by running the software and fixed at the root cause:

1. **A successful \`Status\` converted into a \`Result<T>\` produced a value-less result that reported
   itself as a failure.** Phase helpers returning a bare "ok" status silently truncated the handoff.
   \`Result<T>\` now converts that mistake into a loud \`internal\` failure, and every call site returns
   an explicit document.
2. **The controller re-entered its own mutex.** Public operations acquired the lock and then called
   one another. The type is now split into public entry points and \`*_locked\` implementations that
   never acquire it, and the mutex is non-recursive so a regression fails immediately.
3. **The node re-entered its own mutex** when answering \`node.status\` and when installing a fault.
4. **The RPC client dropped every response body that was not a JSON object**, silently emptying
   array-valued answers such as the authority and epoch documents.
5. **A lease being fenced could be renewed**, which would have resurrected an incarnation the
   controller had ordered to stop and could have produced two authoritative owners.
6. **The mixed-version operation budget never counted anything** when the window opened at log
   position zero, because zero is the "unset" sentinel; the budget is now driven by the open window.
7. **A predecessor that reported itself fenced did not release the mutation slot**, because the
   outstanding fence in the registry was never acknowledged, which blocked the successor.
8. **A fenced predecessor could not serve a snapshot**, so a successor that restarted after the
   fence could not rejoin. Reads were always safe; only mutation is fenced.
9. **A controller restart lost its endpoint bindings**, so reconciliation could not reach the
   components.
10. **Migration evidence hashed the field it produced**, making every sealed step fail its own
    verification.
11. **A restarted node wrote its initial state only on first write**, so a freshly booted component
    had no durable state to recover.
12. **A record in the log beyond the committed log position was replayed as accepted** after a crash
    mid-batch. The state file is now the commit point and uncommitted records are dropped.
13. **The command-line parser let a boolean flag swallow the following positional argument**, so
    \`--json status\` printed usage.
14. **The test harness compared through a dangling reference** when a check ended in \`.value()\` on a
    temporary optional, silently comparing to an empty string.
15. **The test harness leaked build artefacts into the repository tree** and left child processes
    running; scratch directories now remove themselves.
16. **The process harness never closed its child process handles and let every child inherit every
    inheritable handle in the parent.** Both are real resource leaks; children now inherit only their
    output pipe through an explicit handle list.
17. **Scratch directory names could collide between concurrently running test processes**, so one run
    could delete another run's durable state mid-test. Names now include the process id and a random
    per-process nonce; three full suites now run concurrently without interference.

## 4. Validation runs performed for this release

| Run | Result |
| --- | --- |
| Debug, \`/W4 /WX\`, MSVC 19.44 | 137/137 tests pass, zero warnings; per-test debug-heap validation and leak checking enabled |
| Release, \`/W4 /WX\`, MSVC 19.44 | 137/137 tests pass, zero warnings |
| \`ctest\` (full suite, property repeat=3, concurrency, e2e, package consumer) | 5/5 CTest cases pass in Debug and Release |
| Property suite repeated three times | passes |
| Benchmarks | see README |
| Downstream \`find_package\` consumer | configures, builds and runs against the installed tree |

### AddressSanitizer

\`FABRIC_EVOLUTION_ENABLE_ASAN=ON\` is supported and is the recommended configuration where the
toolchain ships the matching runtime. The Visual Studio installation used for this release does
**not** include the x64 AddressSanitizer component (\`clang_rt.asan_dynamic_runtime_thunk-x86_64.lib\`
is absent), so link with \`/fsanitize=address\` fails there. The Debug configuration compensates with:

* \`/RTC1\` runtime checks (stack frame and uninitialised-variable checking),
* \`_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF)\` plus a full
  \`_CrtCheckMemory()\` heap validation after **every** test case,
* leak reporting at process exit.

This is the same class of check ASan performs for heap corruption and use-after-free *within the
allocator's granularity*; it is not a substitute for ASan's redzones and shadow memory, and it is
not claimed to be.
