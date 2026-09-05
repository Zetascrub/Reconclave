# Capability registry

Capability identifiers are lowercase dotted namespaces. Every remotely
available action must have a documented input, output, permission class, and
scope behaviour.

Capability discovery is data-driven. The string in `capabilities` remains the
portable lookup key; an optional matching descriptor supplies version, trust
permission, feature flags, scheduling weight, and concurrency. Coordinators
must select on those fields and current resources, never on a table of hardware
model names. A new board therefore becomes usable by implementing a transport,
advertising standard capability IDs, and accurately describing its limits. It
does not require a coordinator firmware change unless it introduces a genuinely
new capability contract.

| Capability | Initial provider | Permission | v0.1 status |
|---|---|---|---|
| `system.info` | All nodes | Read-only, non-sensitive | Contract defined |
| `net.discovery.scan` | Cardputer, PoE-P4, desktop | Assessment + network scope | Reserved |
| `net.tcp.connect` | Cardputer, PoE-P4 | Assessment + host/port scope | Reserved |
| `net.connectivity.check` | PoE-P4 | Read-only network health | Implemented |
| `net.arp.snapshot` | PoE-P4 | Read-only neighbour cache | Implemented |
| `radio.ble.scan` | Cardputer | Assessment | Reserved |
| `radio.lora.send` | Cardputer | Explicit transmit | Reserved |
| `location.gps.read` | Cardputer | Sensitive location | Reserved |
| `input.keyboard` | Cardputer | Local only | Reserved |
| `vision.camera.capture` | K230 | Sensitive image capture | Reserved |
| `vision.ocr` | K230 | Sensitive evidence processing | Reserved |
| `coordination.job.cancel` | All job-executing nodes | Assessment | Reserved |
| `storage.evidence.write` | Cardputer, desktop | Local evidence storage | Reserved |
| `tool.nmap.services` | Desktop | Assessment + network scope | Implemented |
| `tool.dns.lookup` | Desktop | Assessment (not signed-scope enforced — see below) | Implemented |
| `tool.tcpdump.capture` | Desktop | Assessment (not signed-scope enforced — see below) | Implemented |
| `tool.job.status` | Desktop | Read-only, non-sensitive | Implemented |
| `tool.job.cancel` | Desktop | Assessment | Implemented |

`net.discovery.scan` accepts an optional bounded scope with `network`,
`start_ip`, and `end_ip`. Coordinators use these fields to allocate
non-overlapping chunks. Providers must reject ranges outside their directly
attached IPv4 subnet or larger than a `/24`. Results use `checked`, `total`,
`hosts`, and `job_status`; running jobs are queried through
`coordination.job.status`.

Reserved capabilities may be advertised during development but must not accept
remote execution until their permission and scope enforcement is implemented.

## Packaged tool capabilities

`tool.nmap.services`, `tool.dns.lookup`, and `tool.tcpdump.capture` are safe,
schema-bound adapters (`tools/desktop-node/tool_runner.py`) matching the
tool-runner safety contract in `docs/platform-roadmap.md`: fixed shell-free
argument construction, bounded inputs, Bubblewrap process/PID isolation, and
a best-effort CPU/memory ceiling (a cgroup v2 leaf, a user `systemd-run
--scope`, or POSIX rlimits, tried in that order — each job reports back which
mechanism, if any, actually applied). Each is advertised only when its
executable and Bubblewrap are both discovered installed, matching the "tool
availability is discovered, never assumed" principle.

Unlike `net.discovery.scan`, tool execution is asynchronous: invoking any of
the three returns a job descriptor immediately (`job_id`, `job_status`, and a
`resource_ceiling` object) rather than blocking until the tool exits. Poll
with `tool.job.status` (`{"job_id": ...}`) and stop a running job with
`tool.job.cancel` — idempotent, like `coordination.job.cancel` — which
terminates the whole sandboxed process group (SIGTERM, a grace period, then
SIGKILL) rather than trusting the tool to honour a signal. This is a
separate, shared job registry from `net.discovery.scan`'s single-slot scan
state, so a node can run a tool job and a discovery scan concurrently.

`tool.dns.lookup` and `tool.tcpdump.capture` target hostnames and an optional
local capture filter respectively, neither of which fits the IP-subnet-only
scope-delegation contract described above under "Delegated scope tokens".
They are execution-key authenticated like every other capability in this
section, but — unlike `net.discovery.scan`/`tool.nmap.services` — accepting a
request does not additionally require or verify a delegated scope token yet
(`reconclave_node.py`'s `SCOPE_REQUIRED_CAPABILITIES`). See the "Non-IP scope
delegation" entry in `docs/platform-roadmap.md`'s open design decisions.

## Authenticated capabilities

`net.discovery.scan`, `coordination.job.cancel`, and `storage.evidence.write` can
assess networks, write persistent state, or stop someone else's job, so unlike the read-only capabilities they require a signed,
replay-checked request rather than the "reserved" development posture above. A node
must not advertise or accept one until its trust domain is configured. Network scans
and job control use an **execution key**; evidence writes and receipts use a separate
**evidence key**. Passphrases are entered identically on the coordinator and provider
(Cardputer: Settings > Trust; desktop: `--execution-key` and `--evidence-key`). Both
sides derive a 32-byte key via SHA-256 of the UTF-8 passphrase; the passphrase itself
is never stored, only its digest. A physical pairing transport may provision a root
relationship and derive these domains rather than requiring typed passphrases.

Every request to these capabilities carries an `auth` object alongside the usual
envelope:

```json
{
  "capability": "storage.evidence.write",
  "request_id": "evidence-42",
  "arguments": { "evidence": { "...": "..." } },
  "auth": { "nonce": "3f9a1c2b7e4d5f60", "payload_digest": "sha256...", "tag": "9b2e...c1" }
}
```

`tag` is a 16-byte HMAC-SHA256, truncated, over the canonical string
`source_node|destination_node|request_id|capability|boot_nonce|payload_digest|nonce`,
hex-encoded. `payload_digest` is the lowercase SHA-256 of the compact JSON
encoding of `arguments`. Provisioned coordinator requests append their priority
and lease duration. Authenticated responses use the corresponding compact
`result` or `error` digest, binding returned findings as well as status metadata.
The `boot_nonce` is the target's 128-bit value from its current announcement, so
a captured request cannot be replayed after the target restarts. The receiver
verifies the tag and rejects the request with `UNAUTHENTICATED` if it's missing,
wrong, or if `nonce` has already been accepted from that `source_node` (each receiver
keeps a small bounded set of recently accepted nonces per source, rather than relying
on the envelope's `sequence`, which resets to 1 whenever the sender reboots). Only the
request is authenticated this way — a forged response is a separate, smaller risk not
covered here.

### Delegated scope tokens

Target-bearing dispatch of `net.discovery.scan` (and, on the desktop provider,
`tool.nmap.services`) additionally requires a delegated scope token at
`arguments._scope_delegation`, minted by `EngagementPolicy.delegate` against a
signed engagement scope revision and bound to one destination node, one
capability, and the exact argument set it accompanies:

```json
{
  "scope_id": "scope-...", "project_id": "PR001193", "capability": "net.discovery.scan",
  "destination_node": "rc-p4-01", "included_networks": ["192.168.8.0/24"],
  "excluded_networks": [], "capability_classes": ["discovery"],
  "arguments_digest": "sha256...", "lease_id": "lease-...",
  "issued_at_ms": 1787688000000, "expires_at_ms": 1787688300000,
  "nonce": "...", "tag": "hmac-sha256 hexdigest, 64 hex chars, untruncated"
}
```

The token is signed with the same execution key as the request-auth `tag` above,
but over `json.dumps(token_minus_tag, sort_keys=True, separators=(",", ":"))` —
alphabetically sorted, not wire order — and `tag` here is the **full** 32-byte
HMAC-SHA256 hex digest, not the 16-byte truncated form used for request/response
auth. `arguments_digest` binds the token to the same sort-keys canonical digest of
`arguments` (minus `_scope_delegation` itself). A provider checks the tag,
`capability`/`destination_node`/`arguments_digest` binding, and that the request's
target network is contained by an `included_networks` entry and does not overlap
any `excluded_networks` entry, before accepting the job — desktop providers via
`ReconclaveNode.verify_scope_delegation`, ESP32 providers via the equivalent
`scopeDelegationValid`/`scope_delegation_valid` in their firmware. ESP32 providers
have no wall-clock time, so they check `issued_at_ms`/`expires_at_ms` only for
internal consistency (a valid, ≤5-minute lifetime) rather than against the current
time; the desktop coordinator's own key custody and the request's boot-nonce-bound
replay protection are what keeps an old token from being usefully replayed there.

## Recurring jobs

Any job-accepting capability may take an optional `schedule` object in its
request `arguments`:

```json
{
  "capability": "net.discovery.scan",
  "arguments": {
    "network": "192.168.8.0/24",
    "project_id": "PR001193",
    "scope_id": "default",
    "scope_revision": 1,
    "schedule": {
      "interval_ms": 300000,
      "after_completion": true,
      "policy": "callback",
      "owner_coordinator": "rc-adv-01",
      "callback_endpoint": "http://192.168.8.20:8766",
      "max_failures": 3
    }
  }
}
```

`after_completion: true` means the interval is measured from the previous
run's completion, not its start, so a slow run never overlaps the next one. A
scheduled job keeps its original `job_id` across every run rather than
minting a new one; `coordination.job.status` additionally reports
`recurring: true` and a `run_count` that increments each time the job
restarts. The job does not become `complete`/`cancelled` until it is
explicitly stopped with `coordination.job.cancel`, which providers must accept
as idempotent — calling it with nothing running still returns `ok`.

`policy` is either `independent` or `callback`. Independent tasks continue from
their durable node-owned definition. Callback tasks verify that the named
coordinator remains reachable after each completed run; checks are bounded and
do not overlap the next run. Three consecutive failures stop the task. Providers
advertise the matching `independent` and/or `callback` feature only when they
implement that policy durably.

## `storage.evidence.write` — Evidence Collector

A node advertising `storage.evidence.write` offers to keep a durable local
copy of evidence it did not necessarily produce itself. The coordinator that
started a job is responsible for sending a copy of each result to every known
node that advertises this capability and has it enabled (including itself, if
it is a collector) — collectors do not poll or subscribe.

Request `arguments`:

```json
{
  "evidence": {
    "job_id": "scan-1842",
    "source_node": "rc-p4-01",
    "target": "192.168.8.25",
    "timestamp_ms": 1787688000000,
    "method": "net.discovery.scan",
    "observation": { "responsive": true }
  }
}
```

`job_id`, `source_node`, `target`, `timestamp_ms`, and `observation` are
required; a provider must reject a record missing any of them or exceeding the
16 KiB payload limit with `INVALID_REQUEST`, and must reject the request with
`CAPABILITY_UNAVAILABLE` when the capability is disabled for that node, or
`UNAUTHENTICATED` per the authenticated-capabilities section above. A
successful write returns `ok`; a storage failure (for example, no SD card
mounted) returns `error` with a stable `error_code`. Collectors append,
they never overwrite or deduplicate — conflicting or repeated evidence is a
fact for correlation, not something to silently discard (design doc §10).

## Deterministic change detection

A coordinator compares each completed run's host set against the previous
completed run's set (kept as a small baseline, persisted where storage is
available) and records the difference as evidence with
`method: "coordination.change.detect"` and `observation: {"change": "appeared"}`
or `{"change": "vanished"}` — one record per host, pushed through the same
Evidence Collector path as any other observation. This is not a capability a
node advertises or answers requests for; it's something the coordinator
computes itself from its own run history, deterministically and without AI,
matching the `ai.change.detect` namespace reserved for a future AI-assisted
version of the same idea. The very first run only seeds the baseline — it
never reports every host as newly "appeared".

This currently only covers a job with one clean "run finished" edge: any
one-shot Scout job, or a recurring job with a single local provider. A
recurring job spread across multiple providers on independent schedules isn't
compared yet, since that needs run-cohort synchronisation across providers
that doesn't exist.

## Conditional operations

The desktop primary coordinator mirrors condition rules in its local workspace,
while capable providers store the authoritative rule in NVS.
Rules select a node, one condition (`dhcp_assigned` or `internet_possible`), and
one built-in playbook (`system_snapshot` or `network_scout`). A false-to-true
condition edge triggers the playbook. Network Scout may then continue on the
provider using the recurring contract above. Rules never contain commands,
scripts, URLs, or arbitrary payload bytes: the allowlist is validated again by
the backend, every node request uses provisioned authentication, and all output
is attached to the selected project. Providers expose authenticated
`automation.rule.put`, `automation.rule.list`, and `automation.rule.delete`.
Autonomous evidence is retained through cold boots and synchronised using
`evidence.outbox.read` followed by `evidence.outbox.ack`; collectors must use a
deterministic source-node/sequence ID before acknowledging it. Because
`evidence.outbox.read` is a trusted capability, its response is already signed
with the requesting coordinator's execution key and verified before the caller
ever sees it (`Coordinator._dispatch`); the desktop coordinator carries that
verified `{nonce, tag}` forward onto every evidence record ingested from the
same read as a `provenance` field, rather than discarding it once the transport
check passes. This is a batch-level proof (one signature covers every record in
that read — the provider has no separate per-record evidence key independent of
the pulling coordinator's own execution key), not a per-record one. Evidence
captured without a signed provenance claim (every pre-existing record, and
anything entered directly through the operator API) simply has no `provenance`
field; `provenance`, when present, is folded into the record's content hash, so
forging or swapping it is caught the same way any other tampering is.
