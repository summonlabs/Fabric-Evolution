# Fabric Evolution — operations

## 1. Processes

### Control-plane component

\`\`\`sh
fabric-evolution-node \
  --component node-a --shard shard-0 --state-dir /var/lib/fx/a \
  --host 127.0.0.1 --port 0 \
  --software 1.0.0 --protocol 1.0 --schema 1 \
  --features snapshot_transfer,incremental_catch_up,schema_migration,protocol_negotiation,fenced_stale_rejection,epoch_attestation \
  --lease-ttl-ms 6000
\`\`\`

| Option | Meaning |
| --- | --- |
| \`--component\`, \`--shard\` | Component and shard identity |
| \`--state-dir\` | Durable state directory; **exclusively locked** for the process lifetime |
| \`--host\`, \`--port\` | Bind address; port 0 selects an ephemeral port |
| \`--software\`, \`--protocol\`, \`--schema\` | Versions this build advertises |
| \`--features\` | Comma-separated feature names this build offers |
| \`--lease-ttl-ms\` | Lease lifetime used when the controller grants authority |
| \`--run-ms\` | Exit after N milliseconds instead of serving until killed |

The first line of stdout is machine readable:

\`\`\`
fabric-evolution-node ready component=node-a shard=shard-0 port=54321 incarnation=node-a/1/9f2c… boot=1
\`\`\`

A component keeps its endpoint across restarts when it is restarted on the same port, which is what
the reference manifests expect.

### Controller

\`\`\`sh
fabric-evolution-controller --state-dir /var/lib/fx/controller \
  --id evolution-controller --host 127.0.0.1 --port 0 \
  --compat /etc/fx/compatibility.json \
  --evidence-policy exact --lease-ttl-ms 6000
\`\`\`

| Option | Meaning |
| --- | --- |
| \`--state-dir\` | Durable controller state; exclusively locked |
| \`--compat\` | Compatibility registry document (JSON). Without it the registry is empty and every plan is refused |
| \`--evidence-policy\` | \`exact\` (default) requires the manifest's evidence digest to match the registry now; \`refresh\` accepts a refreshed decision when the verdict is unchanged |
| \`--lease-ttl-ms\` | Authority lease lifetime; must exceed the longest expected phase |

First line: \`fabric-evolution-controller ready controller=evolution-controller port=54322 registry=…\`

### CLI

\`\`\`sh
fabric-evolution --controller 127.0.0.1:54322 [--json] <command> [options]
\`\`\`

| Command | Effect |
| --- | --- |
| \`plan --manifest FILE\` | Admit a sealed manifest; verifies digest and compatibility evidence |
| \`preflight\` | Contact both components, negotiate, verify migration steps, establish predecessor authority |
| \`start\` | Run the handoff to completion |
| \`advance\` | Perform exactly one handoff phase |
| \`pause\` / \`resume\` | Pause at a safe phase boundary / resume |
| \`abort\` | Roll back while safe; automatically continue forward once an irreversible boundary is crossed |
| \`reconcile\` | Re-derive controller state from component reports |
| \`status\` | Campaign, handoff, mixed-version window, authority summary |
| \`handoff\`, \`epoch\`, \`authority\` | Inspection documents |
| \`explain --topic T\` | \`campaign\`, \`authority\`, \`handoff\`, \`epoch\`, \`window\`, \`compatibility\` |

Exit codes: \`0\` success, \`1\` refusal (with the explanation printed), \`2\` argument error, \`3\` local
input error, \`4\` transport error.

## 2. Compatibility registry document

\`\`\`json
{
  "registry": "fabric-compatibility-registry",
  "decisions": [
    {
      "decision_id": "compat-1.0.0-2.0.0",
      "registry": "fabric-compatibility-registry",
      "subject": "node-a",
      "source": "1.0.0",
      "target": "2.0.0",
      "source_protocol": "1.0",
      "target_protocol": "1.1",
      "source_schema": 1,
      "target_schema": 2,
      "verdict": "supported_with_constraints",
      "rollback_permitted": true,
      "max_mixed_version_operations": 10000,
      "max_mixed_version_ms": 600000,
      "certified_features": ["snapshot_transfer", "incremental_catch_up", "schema_migration",
                             "protocol_negotiation", "fenced_stale_rejection", "epoch_attestation"],
      "constraints": [{"name": "mixed_version_window", "value": "bounded"}],
      "decided_at": 1,
      "evidence_digest": "…" 
    }
  ]
}
\`\`\`

\`evidence_digest\` is optional; when omitted the decision is sealed on load. When present it is
verified, and any mismatch is a load error. Allowed verdicts: \`unsupported\`,
\`supported_with_constraints\`, \`supported\`.

## 3. Evolution manifest

\`\`\`json
{
  "id": "manifest-node-a-to-b",
  "campaign": "campaign-1",
  "shard": "shard-0",
  "revision": 1,
  "source": { "id": "node-a", "software": "1.0.0", "protocol": "1.0", "schema": 1,
              "features": ["snapshot_transfer", "incremental_catch_up"],
              "artifact_digest": "…", "host": "127.0.0.1", "port": 54321 },
  "target": { "id": "node-b", "software": "2.0.0", "protocol": "1.1", "schema": 2,
              "features": ["snapshot_transfer", "incremental_catch_up"],
              "artifact_digest": "…", "host": "127.0.0.1", "port": 54322 },
  "compatibility": { "…the decision document above…" },
  "window": { "max_duration_ms": 600000, "max_operations": 10000,
              "min_participants": 2, "require_read_only_shared_phase": true },
  "migrations": [
    { "id": "step-rename-region", "from": 1, "to": 2, "function": "rename_field",
      "parameters": {"from": "region", "to": "zone", "optional": "true"},
      "deterministic": true, "reversible": true, "irreversible_boundary": false,
      "evidence_digest": "…", "description": "rename the region field" }
  ],
  "protocol": { "accepted": ["1.0", "1.1"],
                "required_features": ["snapshot_transfer", "incremental_catch_up"],
                "allowed_features": ["snapshot_transfer", "incremental_catch_up"] },
  "rollback": { "kind": "rollback_if_no_boundary_crossed",
                "recovery_plan": "restore the predecessor before any boundary is crossed",
                "declared_irreversible_boundaries": [] },
  "digest": "…"
}
\`\`\`

The \`digest\` may be omitted; the manifest is then sealed on admission. When present it is verified
and a mismatch is refused.

### Migration functions

| Function | Parameters | Reversible |
| --- | --- | --- |
| \`identity\` | — | yes |
| \`add_field\` | \`key\`, \`value\` | yes (reverse removes the field) |
| \`rename_field\` | \`from\`, \`to\` | yes |
| \`wrap_value\` | \`key\`, \`prefix\`, \`suffix\` | yes |
| \`unwrap_value\` | \`key\`, \`prefix\`, \`suffix\` | yes |
| \`remove_field\` | \`key\` | no |
| \`lowercase_field\` | \`key\` | no |

Any function may take \`"optional": "true"\`, which turns "the field is absent" from a migration
failure into a no-op.

### Rollback strategies

| Kind | Meaning |
| --- | --- |
| \`rollback_to_predecessor\` | Rollback is always safe; refused if the path crosses an irreversible boundary |
| \`rollback_if_no_boundary_crossed\` | Roll back until a boundary is crossed, then go forward |
| \`forward_recovery_only\` | Rollback is never safe; the manifest must describe the recovery plan and must actually cross a boundary |

## 4. Operating a campaign

\`\`\`sh
fabric-evolution --controller 127.0.0.1:54322 plan --manifest evolution.json
fabric-evolution --controller 127.0.0.1:54322 preflight
fabric-evolution --controller 127.0.0.1:54322 start
\`\`\`

\`start\` drives every phase. To step through phase by phase — for a change window, or to inspect
between phases — use \`advance\` repeatedly; each call performs exactly one phase and returns the
status document.

**Pausing.** \`pause\` is honoured at a phase boundary. Inside the authority-transfer window it is
*deferred*, with the reason stated: pausing there could leave the shard without an owner.

**Aborting.** \`abort\` rolls back while no irreversible boundary has been crossed: authority is
restored to the predecessor and the successor is retired. Once a boundary has been crossed, or
authority has already moved, the campaign switches to forward recovery instead and runs to
completion. The decision, with the rule that produced it, is in the returned explanation.

**Restarting a component.** Restart it on the same port. It comes back as a new incarnation with a
higher boot counter and no live authority; the controller notices on the next \`advance\` or
\`reconcile\` and either re-grants authority, restarts the handoff for the new incarnation, or
revokes the dead incarnation's lease, depending on the phase.

**Restarting the controller.** Start it again with the same \`--state-dir\`. It reloads its durable
state, marks every lease unconfirmed, and on \`reconcile\` re-derives the situation from component
reports. Then \`resume\` or \`start\` continues the campaign.

## 5. Troubleshooting

| Symptom | Meaning and action |
| --- | --- |
| \`compatibility_insufficient\` at \`plan\` | The registry has no decision for the pair, the decision is unsupported, or the evidence digest moved. Compare \`explain --topic compatibility\` with the registry document |
| \`feature_not_negotiated\` at \`preflight\` | A required feature is missing from a participant, or is not certified by the registry |
| \`unsupported_protocol\` | Protocol majors differ, or the negotiated version is off the manifest path |
| \`authority_conflict\` | A mutating authority already exists. The incumbent must be fenced and acknowledge, expire, or be provably replaced |
| \`stale_generation\` / \`stale_authority\` / \`stale_incarnation\` | A claim or a replicated record carries an older generation, token or incarnation. This is the fencing working as designed |
| \`not_ready\` with a deferral reason | A pause was requested inside the authority window, a fence is outstanding, or the mixed-version budget is exhausted. The reason is stated verbatim |
| \`irreversible_boundary\` | A downgrade across a declared irreversible step, or an undeclared boundary in the manifest |
| \`integrity_failure\` | A snapshot, record, identity file or state file failed verification. Nothing is silently repaired |
| Component refuses to boot with "state directory is already owned" | Another process holds it. Kill the old process; the lock is the single-writer guarantee |

## 6. Backups and disaster recovery

Copy the whole state directory only while the component is stopped; the directory is exclusively
locked while it runs. An integrity file keeps its previous generation alongside it
(\`state.fxi.prev\`), which the runtime uses only when explicitly asked to recover — and it reports
that it did so.
