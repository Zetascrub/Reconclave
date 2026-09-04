# Reliability contracts

These contracts apply before Reconclave expands into deeper assessment work.

## Participation

A device has one of three operator-selected participation modes:

- `node`: executes enabled local capabilities but does not allocate work to peers;
- `coordinator`: discovers and allocates work but does not accept assessment jobs;
- `both`: provides and coordinates work.

All devices retain the protocol `node` identity role because it means protocol
participant. The `coordinator` role is only advertised in coordinator and both
modes. Capability announcements are runtime truth: coordinator-only mode must not
advertise assessment handlers.

## Durable recurring tasks

Recurring tasks belong to the executing node and use stable task IDs. A task record
contains the capability and arguments, named project/scope IDs, interval measured
after completion, policy, owner coordinator, next run, run count, and last outcome.

- `independent`: continues until paused, cancelled, expired, or blocked by scope or
  storage policy.
- `callback`: renews a coordinator lease after each run. A node retries callback
  delivery with bounded exponential backoff and stops the task after three
  consecutive failures. A coordinator acknowledgement renews the lease.

Tasks must survive reboot in non-volatile storage. Providers expose
`coordination.task.create`, `.list`, `.get`, `.pause`, `.resume`, and `.delete`.
Creating a task is idempotent by task ID.

## Projects and engagement scopes

Coordinators store projects under `/reconclave/projects/<project-id>/`. Each project
has a manifest, one or more named immutable scope revisions, task definitions,
evidence, and an audit log. A job may reference a project and scope revision or carry
an inline scope. The resolved scope is copied into the job record so later project
edits cannot silently broaden running work.

Every provider independently validates allowlisted CIDRs, excluded CIDRs/addresses,
expiry, and requested operation before execution. A project name is convenient UI;
it is not authority without the resolved signed scope.

## Trust domains

Execution control and evidence custody use separate derived keys. A physical Grove,
USB, serial, BLE, or QR exchange provisions one root relationship; HKDF-style labels
derive `reconclave-execution-v1` and `reconclave-evidence-v1` keys. Compromise or
rotation of one domain need not grant the other permission. Physical pairing is a
transport adapter, not a requirement of the wire protocol.

## Store-and-forward evidence

Evidence receives a stable `evidence_id` at creation. A sender retains its local
record and an outbox entry until each required collector returns a signed receipt for
that ID. Retries are idempotent; collectors return the same receipt for duplicates.
The outbox survives reboot and uses microSD when available, otherwise bounded NVS or
other non-volatile storage.

Providers expose storage pressure in node resources and enforce low/high watermarks.
At the low watermark new jobs pause before producing evidence. They resume only after
successful offload raises free space above the high watermark. Existing evidence is
never deleted merely to start another job. If no durable write is possible, the job
fails closed with `STORAGE_PRESSURE` rather than producing untracked observations.
