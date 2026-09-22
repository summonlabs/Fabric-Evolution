# Fabric Evolution — protocol

Everything the runtime sends crosses a socket as a frame, and every frame's payload is a JSON
document. This document specifies both layers.

## 1. Frame layout

\`\`\`
offset  size  field
0       4     magic "FXF1"
4       1     format version (1)
5       1     flags (bit 0: response, bit 1: compressed — reserved, never set)
6       2     reserved, zero
8       4     payload length, little endian
12      4     header CRC-32 (over bytes 0..11)
16      4     payload CRC-32
20      N     payload
\`\`\`

The header is 20 bytes. Decoding is a strict state machine:

* input may be fragmented arbitrarily; the decoder buffers until a frame is complete;
* a bad magic, an unsupported version or a bad header checksum is a protocol violation;
* a declared payload length beyond the configured limit is refused **before** allocating;
* a payload whose CRC does not verify is an integrity failure;
* the decoder buffer is bounded; exceeding it resets the decoder and reports \`bounds_exceeded\`;
* end of stream with a partial frame buffered is reported, never ignored.

Default limits: 1 MiB payload, 4 MiB decoder buffer. Both are configurable per endpoint.

## 2. Request and response

Request:

\`\`\`json
{ "id": "cli", "op": "node.write", "body": { } }
\`\`\`

Response:

\`\`\`json
{ "id": "cli", "op": "node.write", "status": { "code": "ok", "message": "", "detail": { } }, "body": { } }
\`\`\`

\`status.code\` is one of the error codes below; \`message\` is human readable and \`detail\` is a
structured document that states exactly what was compared. The body may be **any JSON value** —
several operations answer with an array.

### Error codes

\`ok\`, \`invalid_argument\`, \`malformed\`, \`bounds_exceeded\`, \`not_found\`, \`already_exists\`,
\`conflict\`, \`stale_epoch\`, \`stale_generation\`, \`stale_incarnation\`, \`stale_authority\`,
\`not_authoritative\`, \`authority_conflict\`, \`compatibility_insufficient\`,
\`irreversible_boundary\`, \`unsupported_protocol\`, \`feature_not_negotiated\`, \`migration_failure\`,
\`integrity_failure\`, \`io_failure\`, \`cancelled\`, \`shutting_down\`, \`deadline_exceeded\`,
\`protocol_violation\`, \`duplicate_frame\`, \`not_ready\`, \`illegal_transition\`,
\`resource_exhausted\`, \`unsupported\`, \`internal\`.

\`not_ready\`, \`conflict\`, \`deadline_exceeded\`, \`resource_exhausted\` and \`io_failure\` are marked
retryable; everything else requires a state change before a retry can succeed.

## 3. Node operations

Identity types appear in JSON as:

\`\`\`json
"incarnation": { "component": "node-a", "boot": 3, "uuid": "9f2c…(32 hex)" }
"claim": { "shard": "shard-0", "incarnation": {…}, "epoch": 1, "generation": 4, "token": "…(32 hex)" }
\`\`\`

| Operation | Body | Answer |
| --- | --- | --- |
| \`node.hello\` | — | \`{hello}\` — protocol hello: identity, versions, features, epoch, generation, whether it holds authority |
| \`node.status\` | — | \`{report}\` — the component's attestation of itself |
| \`node.prepare\` | \`handoff\`, \`role\` (\`predecessor\`\\|\`successor\`), \`manifest_digest\`, and for a successor \`target_software\`, \`target_protocol\`, \`target_schema\`, \`source\` binding | confirmation; refuses when the component does not run the manifest target or when the source schema is ahead of its own |
| \`node.snapshot\` | \`handoff\` | \`{snapshot:{id, lsn, schema, digest, fields}}\`; allowed for a fenced but not retired component |
| \`node.install_snapshot\` | \`handoff\`, \`snapshot\` | refused unless the digest verifies and the schema matches the prepared source schema |
| \`node.read_since\` | \`from_lsn\`, \`max_records\` | \`{records, lsn, oldest_lsn, truncated}\` |
| \`node.apply_records\` | \`handoff\`, \`source\` binding, \`records\` | applies a contiguous batch; refused unless every record matches the prepared binding and is the next expected log position |
| \`node.readiness\` | \`handoff\`, \`target_lsn\`, \`target_schema\` | \`{ready, reason, lsn, schema, digest}\` |
| \`node.fence\` | \`handoff\`, \`fence_id\`, \`target\` | \`{acknowledged, idempotent, fence_id}\`; idempotent for the same fence id |
| \`node.grant_authority\` | \`handoff\`, \`epoch\`, \`generation\`, \`token\`, \`mode\`, \`ttl_ms\` | installs a lease; refused for a retired component |
| \`node.reattest\` | \`token\` | re-confirms a restored lease; refused when the token does not match or the lease expired |
| \`node.migrate\` | \`handoff\`, \`steps\` | applies declared migration steps and journals them |
| \`node.retire\` | \`handoff\` | retires the component; terminal |
| \`node.write\` | \`claim\`, \`key\`, \`value\` | commits durably and answers \`{lsn, epoch, generation, incarnation}\` |
| \`node.read\` | \`key\`, optional \`allow_stale\` | \`{found, value, lsn, authoritative, lifecycle, incarnation, epoch, generation, schema, state_digest}\` |
| \`node.faults\` | \`op\`, \`action\`, \`count\` | installs a bounded fault rule (\`error\` or \`drop_response\`) for failure testing |

A \`node.write\` is refused unless the presented claim matches the component's live lease exactly —
incarnation, epoch, generation and token — the lease is unexpired, and the component is neither
fenced nor retired. \`authoritative\` in a read answer says whether the answer came from a live
authority, so a client can tell a current answer from a stale one instead of guessing.

## 4. Admin operations

The controller's endpoint answers the same envelope with these operations. Every operation that
makes a decision returns an \`explanation\` alongside the status document.

| Operation | Body | Answer |
| --- | --- | --- |
| \`plan\` | \`manifest\` | status document + explanation |
| \`preflight\`, \`start\`, \`advance\`, \`run\`, \`pause\`, \`resume\`, \`abort\`, \`reconcile\` | — | status document + explanation |
| \`status\` | — | campaign, handoff, window, authority, component reports, controller identity |
| \`handoff\`, \`authority\`, \`epoch\` | — | inspection documents |
| \`explain\` | \`topic\` | \`campaign\`, \`authority\`, \`handoff\`, \`epoch\`, \`window\`, \`compatibility\` |

### Explanation document

\`\`\`json
{
  "decision": "campaign.pause",
  "outcome": "deferred",
  "policy": "fabric-evolution.campaign.v1",
  "inputs": { "campaign_state": "running", "action": "pause", "handoff_phase": "predecessor_fenced" },
  "evidence": { },
  "selected_action": "defer_pause",
  "resulting_state": "running",
  "authority": { "epoch": 1, "generation": 3, "actor": { "component": "evolution-controller", … } },
  "rejected_alternatives": [ { "action": "pause", "reason": "authority is mid-transfer; …" } ],
  "notes": [ ],
  "decided_at_ms": 1790106293586
}
\`\`\`

Keys are emitted in canonical (ascending byte) order, so two runs with the same inputs produce
byte-identical explanations. The same inputs and the same durable state always produce the same
decision: the campaign and handoff state machines are pure functions of their records.
