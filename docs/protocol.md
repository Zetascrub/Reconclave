# Reconclave Protocol v1

The protocol identifier is exactly `reconclave/1`. Firmware versions are
independent. All JSON messages use UTF-8 and the common envelope defined by
`protocol/schemas/envelope.schema.json`.

Required envelope fields are `proto`, `type`, `message_id`, `source_node`,
`timestamp_ms`, `sequence`, and `payload`. `destination_node` is omitted for
broadcast announcements. Sequence numbers are monotonically increasing within
an authenticated session and, with the timestamp and message ID, provide the
inputs for replay detection once session authentication lands.

Announcements include one or more runtime roles. Every participant carries the
`node` role; devices able to originate and manage work additionally advertise
`coordinator`, and heavier analysis services may advertise `analysis`. Roles
describe participation while capabilities describe callable behaviour, so the
Cardputer ADV can be both `node` and `coordinator` without becoming a permanent
master.

## Initial capability

`system.info` is the v0.1 proof capability. It is read-only and returns device
type, firmware, uptime, and available memory. It provides no assessment or
mutation behaviour.

## Response rules

- `ok` is a completed synchronous request.
- `accepted` must include a `job_id`; later state arrives in events.
- `rejected` means policy, capability, scope, or request validation denied it.
- `error` means execution failed after the request was otherwise valid.
- Error responses carry a stable machine-readable `error_code`.
- A job started with a `schedule` argument (see `docs/capabilities.md`) keeps
  its `job_id` across every run and stays out of `complete`/`cancelled` until
  `coordination.job.cancel` explicitly stops it.

Unknown protocol versions, message types, capabilities, and malformed fields
are rejected. Receivers must apply transport-specific frame size limits before
decoding; the portable v1 library caps a decoded payload at 16 KiB.

`stream` transfers independently ordered, base64-encoded chunks. The
`stream_id` and zero-based `chunk_index` identify ordering, and `final` closes
the stream. Transport adapters must reject duplicate or skipped indices rather
than silently constructing corrupt evidence.

Run the dependency-free contract check with:

```sh
python3 tools/protocol-debugger/validate_vectors.py protocol/test-vectors/*.json
```

## Security boundary

The schema proves shape, not trust. Until node authentication, integrity,
replay protection, capability permissions, and engagement-scope enforcement
are implemented, transports must expose only benign information capabilities.

`coordination.job.cancel` and `storage.evidence.write` are the first exception:
they are not benign, so they carry their own per-request signature and replay
check (a shared-passphrase HMAC, not the general session authentication this
section otherwise describes) — see "Authenticated capabilities" in
`docs/capabilities.md`. Every other non-benign capability remains reserved
until the general mechanism referenced above lands.
