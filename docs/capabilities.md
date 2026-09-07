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
| `tool.masscan.services` | Desktop | Assessment + network scope | Implemented (unverified — see below) |
| `tool.arpscan.sweep` | Desktop | Assessment (local L2 segment only) | Implemented (unverified — see below) |
| `tool.dns.lookup` | Desktop | Assessment (not signed-scope enforced — see below) | Implemented |
| `tool.tcpdump.capture` | Desktop | Assessment (not signed-scope enforced — see below) | Implemented |
| `tool.job.status` | Desktop | Read-only, non-sensitive | Implemented |
| `tool.job.cancel` | Desktop | Assessment | Implemented |
| `fleet.ota.apply` | Cardputer, PoE-P4 | Trusted, two-phase (see below) | Implemented |

`net.discovery.scan` accepts an optional bounded scope with `network`,
`start_ip`, and `end_ip`. Coordinators use these fields to allocate
non-overlapping chunks. Providers must reject ranges outside their directly
attached IPv4 subnet or larger than a `/24`. Results use `checked`, `total`,
`hosts`, and `job_status`; running jobs are queried through
`coordination.job.status`.

Reserved capabilities may be advertised during development but must not accept
remote execution until their permission and scope enforcement is implemented.

## Packaged tool capabilities

`tool.nmap.services`, `tool.masscan.services`, `tool.arpscan.sweep`,
`tool.dns.lookup`, and `tool.tcpdump.capture` are safe, schema-bound adapters
(`tools/desktop-node/tool_runner.py`) matching the tool-runner safety contract
in `docs/platform-roadmap.md`: fixed shell-free argument construction, bounded
inputs, Bubblewrap process/PID isolation, and a best-effort CPU/memory ceiling
(a cgroup v2 leaf, a user `systemd-run --scope`, or POSIX rlimits, tried in
that order — each job reports back which mechanism, if any, actually applied).
Each is advertised only when its executable and Bubblewrap are both discovered
installed, matching the "tool availability is discovered, never assumed"
principle.

Unlike `net.discovery.scan`, tool execution is asynchronous: invoking any of
these returns a job descriptor immediately (`job_id`, `job_status`, and a
`resource_ceiling` object) rather than blocking until the tool exits. Poll
with `tool.job.status` (`{"job_id": ...}`) and stop a running job with
`tool.job.cancel` — idempotent, like `coordination.job.cancel` — which
terminates the whole sandboxed process group (SIGTERM, a grace period, then
SIGKILL) rather than trusting the tool to honour a signal. This is a
separate, shared job registry from `net.discovery.scan`'s single-slot scan
state, so a node can run a tool job and a discovery scan concurrently.

`tool.nmap.services` accepts an optional `script_category` argument
restricted to a fixed allowlist (`default`, `discovery`, `safe`) — never a raw
script name, and never a category (`vuln`, `auth`, `exploit`, `intrusive`,
`dos`, `external`) that probes for or acts on a weakness rather than
enumerating what's there. Omitting it keeps the adapter's original
connect-scan-only behaviour exactly as before.

`tool.masscan.services` shares its `hosts`/`ports` argument schema and bounds
with `tool.nmap.services` and produces the same `nmap-services/v1` result
shape (masscan's `-oX` output is nmap-compatible XML), so
`vulnerability_analysis.extract_observations` needs no tool-specific branch to
correlate its findings. Unlike nmap's `-sT` connect scan, masscan sends raw
SYN packets and has no kernel-connect equivalent, so it needs `CAP_NET_RAW`
(and `CAP_NET_ADMIN`) on the `masscan` binary itself — Bubblewrap isolation
here does not unshare the user namespace, so a file capability set on the host
binary (`sudo setcap cap_net_raw,cap_net_admin+eip $(command -v masscan)`)
carries through unchanged into the sandbox; without it, a scan simply lands in
the normal "failed" job state with the permission error as `error` text.
`--rate` is fixed at a conservative value and is not an operator-settable
argument — masscan's entire differentiator is scan speed/scale, and this
adapter deliberately declines to expose that knob. `tool.arpscan.sweep` takes
only an `interface` (reusing `tool.tcpdump.capture`'s discovered-interface
validation) and sweeps that interface's own attached subnet; it needs the same
`CAP_NET_RAW` treatment as masscan, and has no host/port targeting at all,
since arp-scan can never see past its own attached L2 segment regardless of
arguments.

**Verification status:** `tool.masscan.services` and `tool.arpscan.sweep`
were built against each tool's documented output format only — neither
`masscan` nor `arp-scan` was installed in the reference dev environment at the
time these adapters were written (unlike `tool.nmap.services`/`tool.dns.lookup`/
`tool.tcpdump.capture`, which were all validated against their real binaries).
Treat both as unverified until run for real, matching the "no real capture to
validate against" caveat already carried by `tool.tcpdump.capture`'s own
output parser.

`tool.dns.lookup` and `tool.tcpdump.capture` target hostnames and an optional
local capture filter respectively, and `tool.arpscan.sweep` can only ever
reach its own attached L2 segment regardless of arguments — none of the three
fit the IP-subnet-only scope-delegation contract described above under
"Delegated scope tokens", or (for arp-scan) need to. All three are
execution-key authenticated like every other capability in this section, but
— unlike `net.discovery.scan`/`tool.nmap.services`/`tool.masscan.services` —
accepting a request does not additionally require or verify a delegated scope
token (`reconclave_node.py`'s `SCOPE_REQUIRED_CAPABILITIES`). See the "Non-IP
scope delegation" entry in `docs/platform-roadmap.md`'s open design decisions
for `tool.dns.lookup`/`tool.tcpdump.capture`.

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

### Standing scope grants (offline autonomy)

Delegated scope tokens above are minted at dispatch time by a live coordinator and
hard-capped at five minutes (`EngagementPolicy.delegate`'s `min(scope["expires_at_ms"],
now + 300000)`), because that coordinator is assumed to be reachable for every
individual request. A device meant to operate autonomously with no coordinator
present — the deferred "local automation engine" phase in
`docs/platform-roadmap.md` — needs a different primitive: a **standing grant**,
minted once (typically at provisioning) via `EngagementPolicy.mint_standing_grant`
and verified locally by the device itself, with no live signer available
afterward.

```json
{
  "grant_id": "grant-...", "scope_id": "scope-...", "project_id": "PR001193",
  "destination_node": "implant-1", "included_networks": ["192.168.8.0/24"],
  "excluded_networks": [], "capability_classes": ["discovery"],
  "duration_ms": 259200000, "minted_at_ms": 1787688000000,
  "nonce": "...", "tag": "hmac-sha256 hexdigest, 64 hex chars, untruncated"
}
```

Unlike a delegated scope token, a standing grant carries a **duration**, not an
absolute `expires_at_ms` — a freshly provisioned device may have no wall clock at
all (matching how ESP32 providers already treat delegated-token timestamps as
internal-consistency-only, not wall time, per "Delegated scope tokens" above).
`mint_standing_grant` requires the requested `capability_classes` to be a subset
of the underlying scope's own approved classes, and caps `duration_ms` at the
smaller of 7 days or the scope's own remaining validity — an offline grant must
not outlive the engagement scope it was derived from, even though nothing will
be present to re-check that scope once the device goes offline. The grant is
signed with the same delegation-key domain and full, untruncated
HMAC-SHA256 hex-digest convention as `_scope_delegation` tokens.

Verification (`EngagementPolicy.verify_standing_grant`) is a `staticmethod`
taking the raw key bytes rather than reading an instance's own
`self.delegation_key`, so a provider holding only its shared execution-key
bytes — not a live `EngagementPolicy`/`WorkspaceStore` — can check a grant on its
own, the same way `ReconclaveNode.verify_scope_delegation` independently
re-derives a delegated token's HMAC rather than calling back into
`EngagementPolicy`. It is also deliberately clock-agnostic: the caller supplies
`elapsed_ms` from whatever local clock it has. On the desktop provider today
that is `ArmedClock` (`tools/desktop-node/reconclave_node.py`) — a small
persisted counter, checkpointed to disk periodically and on clean shutdown, so
elapsed time survives a restart. `ArmedClock` is **fail-closed**: a missing,
unreadable, or `grant_id`-mismatched state file reports elapsed time one
millisecond past the grant's `duration_ms` (i.e. already expired) rather than
zero — a device that lost its bookkeeping must never silently re-arm itself.

As of this writing, this is a **foundation-only primitive**: nothing in the
codebase yet calls `verify_standing_grant` from a capability handler.
`Node.__init__` accepts optional `standing_grant`/`standing_grant_state_path`
arguments purely to construct and start the `ArmedClock`; no autonomous
execution is wired to it. The consumer — a local, no-coordinator-required
automation engine on a Linux companion node — is a deferred phase in
`docs/platform-roadmap.md`.

## Fleet OTA delivery

`fleet.ota.apply` (`FleetManager._apply_release_to_device`, `tools/desktop-node/
fleet_manager.py`) is deliberately two-phase rather than a single authenticated
call like the capabilities above, because a firmware image is a few hundred KB
to a couple MB and neither ESP32 target can safely hold a base64-inflated copy
of that in RAM (poe-p4 has no PSRAM configured; cardputer-adv's 8MB PSRAM could
technically fit one, but the same code path serves both targets):

1. **Arm** — an ordinary authenticated request like any other trusted
   capability above (signed, replay-checked, carries the coordinator's lease).
   Its arguments are only the release descriptor (`device_type`, `version`,
   `artifact_sha256`, `signature`) and a fresh coordinator-minted
   `upload_token` — no artifact bytes. On success the device has opened its
   *inactive* OTA partition for writing and remembers the token and the
   claimed SHA-256; nothing has been written to flash yet beyond that.
2. **Upload** — a second, raw (non-JSON, non-multipart) POST straight to a
   dedicated path (`/reconclave/v1/ota-upload` on both targets) carrying the
   artifact bytes and the token as a query parameter. The token — already
   covered by phase 1's signature — is the only credential this request
   presents; there is no per-chunk auth overhead. The device streams each
   chunk directly into the flash write API (`esp_ota_write` / Arduino
   `Update.write`) while feeding a running SHA-256, so it never buffers the
   whole image. Only once the finished hash matches the value phase 1 signed
   does the device call `esp_ota_set_boot_partition` / rely on `Update.end`'s
   equivalent and reboot; a mismatch, an oversized transfer, or a transport
   error aborts the write and leaves the previously running partition as the
   boot target. The device's JSON reply back is itself authenticated the same
   way a trusted-capability response is: an HMAC tag over
   `device_id|coordinator_id|upload_token|status|artifact_sha256`, verified by
   `Coordinator.upload_artifact` with the same key `invoke()` would have used.

`poe-p4`'s partition table was previously single-app (no OTA slot at all) and
now carries `ota_0`/`ota_1` plus `otadata` (`devices/poe-p4/partitions.csv`),
with `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` + `CONFIG_APP_ROLLBACK_ENABLE` set
in `sdkconfig.defaults` so a new image that never reaches a working network
(and therefore never calls `esp_ota_mark_app_valid_cancel_rollback`, done once
in `got_ip_event`) is reverted automatically by the bootloader. `cardputer-adv`
already shipped with a two-OTA-slot table (`default_8MB.csv`) and gets the same
`esp_ota_mark_app_valid_cancel_rollback` call once its network services start —
but as a PlatformIO Arduino build against a precompiled framework bootloader,
whether that bootloader itself was built with rollback support isn't something
this firmware controls; the call is harmless either way.

`FleetManager.create_release` takes the artifact as `artifact_base64` in
addition to its previously-required `artifact_sha256`, re-hashes the decoded
bytes, and rejects a mismatch. The bytes themselves are stored outside the
workspace's JSON snapshot (`WorkspaceStore.store_ota_artifact`/
`read_ota_artifact`, one file per SHA-256 under `ota_artifacts/`) since that
snapshot is rewritten in full on every unrelated write.

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

On the desktop provider (`ReconclaveNode.write_evidence`), a successful write's
just-verified request signature is carried into the stored record as a
`provenance` field (`source_node`, `verified`, `request_nonce`, `request_tag`,
`algorithm`) — distinct from the record's own self-reported `source_node`
above, which is unauthenticated content describing what the evidence is
about, not who the coordinator cryptographically verified sent the request.
The record itself is stored AES-256-GCM-encrypted at rest
(`encrypted_spool.py` on desktop; `RC_STORAGE_KEY`-based encryption in
firmware on `poe-p4`'s NVS evidence outbox and `cardputer-adv`'s microSD
evidence log — see `docs/platform-roadmap.md` Phase 5), keyed per-device
independent of any coordinator pairing.

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
