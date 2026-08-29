# Capability registry

Capability identifiers are lowercase dotted namespaces. Every remotely
available action must have a documented input, output, permission class, and
scope behaviour.

| Capability | Initial provider | Permission | v0.1 status |
|---|---|---|---|
| `system.info` | All nodes | Read-only, non-sensitive | Contract defined |
| `net.discovery.scan` | Cardputer, PoE-P4, desktop | Assessment + network scope | Reserved |
| `net.tcp.connect` | Cardputer, PoE-P4 | Assessment + host/port scope | Reserved |
| `radio.ble.scan` | Cardputer | Assessment | Reserved |
| `radio.lora.send` | Cardputer | Explicit transmit | Reserved |
| `location.gps.read` | Cardputer | Sensitive location | Reserved |
| `input.keyboard` | Cardputer | Local only | Reserved |
| `vision.camera.capture` | K230 | Sensitive image capture | Reserved |
| `vision.ocr` | K230 | Sensitive evidence processing | Reserved |
| `coordination.job.cancel` | All job-executing nodes | Assessment | Reserved |
| `storage.evidence.write` | Cardputer, desktop | Local evidence storage | Reserved |

`net.discovery.scan` accepts an optional bounded scope with `network`,
`start_ip`, and `end_ip`. Coordinators use these fields to allocate
non-overlapping chunks. Providers must reject ranges outside their directly
attached IPv4 subnet or larger than a `/24`. Results use `checked`, `total`,
`hosts`, and `job_status`; running jobs are queried through
`coordination.job.status`.

Reserved capabilities may be advertised during development but must not accept
remote execution until their permission and scope enforcement is implemented.

## Authenticated capabilities

`coordination.job.cancel` and `storage.evidence.write` can write persistent state or
stop someone else's job, so unlike the read-only capabilities they require a signed,
replay-checked request rather than the "reserved" development posture above. A node
must not advertise or accept either one until it has an **evidence key** configured —
a shared passphrase entered identically on the coordinator and on the provider
(Cardputer: Settings > Trust > Evidence key; desktop node: `--evidence-key`). Both
sides derive a 32-byte key via SHA-256 of the UTF-8 passphrase; the passphrase itself
is never stored, only its digest.

Every request to these two capabilities carries an `auth` object alongside the usual
envelope:

```json
{
  "capability": "storage.evidence.write",
  "request_id": "evidence-42",
  "arguments": { "evidence": { "...": "..." } },
  "auth": { "nonce": "3f9a1c2b7e4d5f60", "tag": "9b2e...c1" }
}
```

`tag` is a 16-byte HMAC-SHA256, truncated, over the canonical string
`source_node|destination_node|request_id|capability|nonce`, hex-encoded. The receiver
verifies the tag and rejects the request with `UNAUTHENTICATED` if it's missing,
wrong, or if `nonce` has already been accepted from that `source_node` (each receiver
keeps a small bounded set of recently accepted nonces per source, rather than relying
on the envelope's `sequence`, which resets to 1 whenever the sender reboots). Only the
request is authenticated this way — a forged response is a separate, smaller risk not
covered here.

## Recurring jobs

Any job-accepting capability may take an optional `schedule` object in its
request `arguments`:

```json
{
  "capability": "net.discovery.scan",
  "arguments": {
    "network": "192.168.8.0/24",
    "schedule": { "interval_ms": 300000, "after_completion": true }
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
