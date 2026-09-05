# Reconclave platform implementation roadmap

This file is the durable checkpoint for the multi-phase assessment-platform build.
Update the status and notes whenever a phase materially changes. The platform is for
explicitly authorised testing. Active exploitation is out of scope; post-exploitation
capabilities require a separate, explicit approval policy and are disabled by default.

## Product decisions

- Support local, VPN, and internet relay deployments.
- Support multiple operators eventually; preserve a simple single-operator mode now.
- Desktop-class nodes may run packaged assessment tools. Tool availability is discovered,
  never assumed from an operating-system label.
- ESP32 devices may be portable vantage points, durable sensors, physical-interface tools,
  relays, or display-only engagement consoles.
- Workflow automation is the first delivery priority.
- Vulnerability analysis will reuse selected ideas and parsers from Icebreaker, including
  safe detectors/analyzers, CVE enrichment, risk scoring, Nessus/NASL import, and reports.
- Relay transport (Phase 8/11) is WireGuard-only: private addressing, no mutually authenticated
  WebSocket/QUIC fallback. Simplest, strongest default trust model; every relay-connected node
  or gateway needs WireGuard.
- Operator identity (Phase 10) starts with local accounts; external OIDC is a later addition, not
  a first-release requirement. Matches "preserve a simple single-operator mode now" above.
- Vulnerability intelligence (Phase 9) stays offline-only: no live CVE/CPE lookups, no online
  enrichment, no hybrid cache. Correlation is limited to CVE identifiers already present in
  imported documents (Nessus/NASL), consistent with the platform's privacy-conscious principle.
- The desktop node is the permanent primary coordinator (signed scopes, workflow dispatch,
  evidence custody, fleet management), not a placeholder for the K230. The K230, once it exists,
  is a capability-rich node like any other rather than a required master — see
  `Reconclave_Design_Document_v0.1.md` §3/§4, whose "K230 as flagship" framing this supersedes.
- Consensus scanning (design doc §9.2) folds into Phase 7's adaptive-scheduling exit criteria
  rather than becoming its own phase.
- Non-IP scope delegation: the scope-delegation token schema will grow a hostname/interface-bound
  variant so `tool.dns.lookup`/`tool.tcpdump.capture` (and future non-IP-targeted adapters) get the
  same signed-scope enforcement `net.discovery.scan` already has, rather than staying
  execution-key-only. Not yet implemented — see Phase 3's open item below.
- Post-exploitation approval policy is deferred until a specific post-exploitation capability is
  actually proposed, rather than designed speculatively ahead of one. Post-exploitation stays
  disabled by default in the meantime (see the top of this file).
- Evidence retention policy is operator-configurable per project, not a fixed platform-wide
  default and not unlimited-by-design. Matches the existing pattern where destructive/retention
  actions (audit pruning) are explicit operator-triggered actions, never automatic.

## Delivery phases

| Phase | Major feature | Status | Exit criteria |
|---|---|---|---|
| 0 | Protocol integrity and reliable P4 evidence | Complete | Authenticated arguments/results; cold-boot rule triggers; live evidence import |
| 1 | Signed engagement scopes and policy engine | Complete | Immutable scope revisions, exclusions, expiry, rate/concurrency limits, provider enforcement |
| 2 | Workflow and campaign engine | Initial slice complete | Durable DAG definitions/runs, dependency scheduling, retries, cancellation, resumability |
| 3 | Capability SDK and packaged tool runners | Complete | Signed manifests, schemas, risk classes, discovery, sandboxed desktop execution |
| 4 | Fleet management and signed OTA | Complete (device firmware unverified on real hardware — see Phase 4 notes) | Health/inventory/config drift, staged rollout, verification and rollback |
| 5 | Evidence pipeline and chain of custody | In progress | Content-addressed records, node MAC/signature, encrypted spool, receipts, export bundle |
| 6 | Live operations timeline | Complete | Correlated campaign/job/node/evidence events with trace IDs and searchable audit history |
| 7 | Adaptive distributed scheduling | Planned | Capability/resource/topology selection, scope sharding, failover and backpressure, consensus scanning (design doc §9.2) |
| 8 | VPN and internet relay | Planned | Mutually authenticated relay, expiring delegation, offline queue, no implicit transitive trust |
| 9 | Vulnerability analysis | Complete | Normalised findings, correlation, confidence, CVE enrichment, safe validation, Nessus import |
| 10 | Collaboration and reporting | Planned | Operators/roles/approvals, notes, comparisons, ATT&CK/STIX/OCSF mappings, report exports |
| 11 | Secure relay/gateway nodes | Planned | Explicit routes, per-hop authority, store-and-forward, route visibility, emergency stop |
| 12 | Edge AI and local analysis tier | Planned, gated on K230 hardware | K230 vision/OCR/classification capabilities, local AI analysis gateway (Ollama/llama.cpp/approved provider), RAG over security knowledge, enforced per-engagement AI privacy policy |

## Phase 2 initial vertical slice

- [x] Persist workflow definitions, workflow runs, and per-step state in the workspace store.
- [x] Validate acyclic dependencies and capability-shaped step types.
- [x] Resolve ready steps against live nodes by capability and optional preferred node.
- [x] Dispatch one step at a time with immutable arguments.
- [x] Persist attempts, results, errors, retry decisions, cancellation state, and timeouts.
- [x] Expose workflow create/run/status/cancel APIs.
- [x] Add a web workflow template and live run view.
- [x] Add unit tests for validation, resume-after-restart, dependency scheduling, retry, and cancellation.
- [x] Bind every workflow run to a signed scope revision before target-bearing dispatch: `POST
  /api/workflows/<id>/runs` now checks whether any step's capability is target-bearing
  (`desktop_app.is_target_bearing`, mirroring `EngagementPolicy`'s own boundary) and rejects the
  run at creation time with no valid `scope_id`, rather than only discovering the gap deep inside
  a failed per-step dispatch attempt later. A workflow with no target-bearing steps still runs
  without a scope.
- [x] Add workflow editing/deletion: `WorkspaceStore.update_workflow`/`delete_workflow` (audited, blocked while
  a run is queued/running so an in-flight run's step-id/definition matching can't be corrupted out from
  under it), `POST`/`DELETE /api/workflows/<id>`, and a minimal reachable UI (rename + remove per
  workflow card, `web/src/WorkspaceViews.tsx`) — web build verified (`npm run build`).
- [ ] Add a general visual builder. Editing today is name/description/steps via a JSON body — there is
  no drag-and-drop or graphical DAG editor for step definitions; that remains a real, separate,
  larger frontend feature.

## Phase 1 scope boundary

- [x] Persist immutable, incrementing scope revisions.
- [x] Sign revisions with a local 256-bit key and reject tampering or expiry.
- [x] Enforce IPv4 inclusions and exclusions by subnet containment.
- [x] Restrict approved operation classes and persist rate/concurrency ceilings.
- [x] Add an operator approval UI and audit records.
- [x] Apply scopes to direct discovery, TCP inspection, and target-bearing workflows.
- [x] Enforce rate/concurrency ceilings with durable dispatch leases.
- [x] Deliver argument-bound, expiring delegated scope tokens and verify them on desktop providers.
- [x] Verify delegated scope tokens on ESP32 providers (cardputer-adv `main.cpp`, poe-p4 `main.c`): the
  token's HMAC tag, capability/destination-node/argument binding, and included/excluded network
  containment are checked before a new `net.discovery.scan` job is accepted, mirroring
  `EngagementPolicy`/`ReconclaveNode.verify_scope_delegation` on the desktop coordinator. **Known
  limitation:** neither device has wall-clock time (no NTP sync), so `issued_at_ms`/`expires_at_ms`
  are checked only for internal consistency (expiry after issue, within the 5-minute lifetime
  ceiling the coordinator enforces when minting tokens) rather than against the current time;
  absolute token freshness still relies on the outer request's boot-nonce-bound replay protection.
  Their existing attached-network bounds remain an additional, independent constraint regardless.

## Phase 3 packaged runners

- [x] Add a discoverable, opt-in Nmap service-enumeration adapter.
- [x] Use fixed argument construction without a shell, bounded IP targets/ports, timeout, and XML parsing.
- [x] Require the trusted execution domain and coordinator scope authorization.
- [x] Sign packaged manifests and bind them to executable paths and SHA-256 identities.
- [x] Add safe adapters for the remaining installed tools and asynchronous cancellation: `tool.nmap.services`
  moved to an async job model (Popen + job-id/status/cancel via new `tool.job.status`/`tool.job.cancel`,
  shared registry keyed by `job_id`), plus `tool.dns.lookup` (dig; validated against real captured
  output) and `tool.tcpdump.capture` (tcpdump; command construction and permission-denied path
  validated against the real binary, output parsing built from documented format only — no real
  capture to validate against in this environment). Nuclei/testssl.sh/sslscan/Nikto/WhatWeb are not
  installed on the reference dev machine (checked via `shutil.which`, per the discovery-not-assumption
  principle) and remain unimplemented pending an environment that has them. **Known gap:** the two new
  adapters target hostnames / an optional local capture filter, which don't fit the current IP-subnet
  scope-delegation contract, so unlike `net.discovery.scan`/`tool.nmap.services` they are execution-key
  authenticated but not yet signed-scope enforced (`reconclave_node.py` `SCOPE_REQUIRED_CAPABILITIES`).
- [x] Require Bubblewrap process/PID isolation, read-only root, and ephemeral `/tmp`.
- [x] Add cgroup CPU/memory ceilings and asynchronous process cancellation: cancellation is idempotent,
  process-group based (SIGTERM, grace period, then SIGKILL), covers all tool jobs. Ceilings are
  best-effort across three mechanisms tried in order — a cgroup v2 leaf, a user `systemd-run --scope`,
  then POSIX rlimits — and each job reports back which mechanism (if any) actually applied rather than
  assuming one worked.

## Phase 4 fleet management and signed OTA

This phase's orchestration logic (`tools/desktop-node/fleet_manager.py`) was already substantially
built before this checklist existed for it — the table above listed it "Planned" while real code was
already wired to live API routes. Corrected here rather than left stale.

- [x] Health/inventory: `FleetManager.reconcile_once` polls the coordinator's live node roster every 10s,
  tracks `first_seen_ms`/`last_seen_ms` per device, and persists the result to `workspace_store`'s
  `fleet_nodes` (survives restart).
- [x] Config drift: `set_config`/`SAFE_CONFIG_KEYS` records a desired-state document per `device_type`
  (labels, telemetry interval, enabled capabilities) and `reconcile_once` diffs it against each live
  node's advertised capabilities, exposed at `GET/POST /api/fleet/config`.
- [x] Signed releases: `create_release` binds a `device_type`, version, and artifact SHA-256 into an
  HMAC-SHA256-signed manifest (`POST /api/fleet/releases`, `operator_authorised` gated).
- [x] Staged rollout: `create_rollout` batches targets by `device_type` (bounded batch size),
  `advance_rollout` dispatches one batch via `fleet.ota.apply` and verifies the returned artifact hash
  before marking a target `verified`; exceeding a 25% failure threshold within a batch flips the rollout
  to `rollback_required` instead of continuing (`POST /api/fleet/rollouts`, `POST /api/fleet/advance/<id>`).
- [x] Rollback: `rollback_rollout` reverts every `verified` target on a rollout back to the release its
  own rollout recorded as `previous_release_id` (the most recent prior completed/partial rollout for the
  same `device_type`), re-verifying the artifact hash on the way back down (`POST /api/fleet/rollback/<id>`,
  `operator_authorised` gated). A rollout with no recorded prior release (the first-ever release for a
  device_type) has nothing to roll back to and rejects the request explicitly rather than guessing.
- [x] Test coverage: `test_fleet_manager.py` (new — this subsystem had zero tests before), covering
  config validation, drift detection, release signing, batched rollout verification/failure/threshold
  behaviour, and rollback including the "nothing to roll back to" and "revert itself fails" cases.
- [x] Web UI: fleet had zero frontend surface before this (no view, no types) despite the API already
  existing — added a `fleet` tab (`web/src/WorkspaceViews.tsx`'s `FleetView`, `web/src/types.ts`'s
  `FleetNode`/`FleetConfig`/`OtaRelease`/`OtaRollout`) showing node inventory/drift, a signed-release
  form, and staged-rollout creation/advance/rollback — the one view that intentionally ignores the
  per-project filter every other tab applies, since fleet spans the whole deployment. Web build and
  lint both verified clean.
- [x] Device-side `fleet.ota.apply`: implemented on both ESP32 targets as a two-phase capability —
  see `docs/capabilities.md`'s "Fleet OTA delivery" section for the full design. Phase 1 arms the
  device over the ordinary authenticated envelope (release descriptor + a fresh upload token, no
  artifact bytes); phase 2 streams the artifact to a dedicated raw-body endpoint
  (`/reconclave/v1/ota-upload`) straight into the inactive OTA partition (`esp_ota_write` on
  poe-p4, Arduino `Update.write` on cardputer-adv) with a running SHA-256, so neither target ever
  buffers a base64-inflated image in RAM (poe-p4 has no PSRAM configured). The boot partition only
  switches once the finished hash matches what phase 1 signed. `poe-p4`'s partition table gained
  `ota_0`/`ota_1`/`otadata` (`devices/poe-p4/partitions.csv`, previously single-app) plus
  `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`/`CONFIG_APP_ROLLBACK_ENABLE`; `cardputer-adv` already had
  a two-OTA-slot table. Both call `esp_ota_mark_app_valid_cancel_rollback` once network services
  come up post-boot. `FleetManager.create_release` now takes the artifact bytes
  (`artifact_base64`), re-hashes and rejects a mismatch, and stores them outside the workspace's
  JSON snapshot (`WorkspaceStore.store_ota_artifact`/`read_ota_artifact`); `advance_rollout`/
  `rollback_rollout` drive both phases via `Coordinator.invoke` + the new `Coordinator.
  upload_artifact`. Test coverage: `test_fleet_manager.py`, `test_workspace_store.py`, and
  `test_coordinator.py` all extended (131 desktop-node tests pass). **Verification limits, stated
  plainly:** `cardputer-adv` (PlatformIO/Arduino, toolchain available in this environment) builds
  clean with `-Wall -Wextra -Wformat=2` and no new warnings. `poe-p4` (ESP-IDF/CMake) could not be
  compiled here — no `idf.py` toolchain in this environment — so its C changes were reviewed by
  hand against the exact vendored API signatures (mbedtls 3.6.7, `esp_ota_ops.h`) rather than
  compiled; treat it as unverified until a real `idf.py build` confirms it. **Neither target has
  been flashed to real hardware or exercised through an actual OTA cycle** — validate on real
  devices, starting with a bench unit, before rolling out to anything already deployed.

## Phase 9 vulnerability analysis

- [x] Normalise imported findings with stable correlation identities, confidence, risk score, and lifecycle timestamps.
- [x] Import `.nessus` report XML and offline NASL plugin metadata without executing plugin code.
- [x] Reject XML entity declarations and bound import size.
- [x] Add project finding persistence, deduplication, audit events, API, and operator UI.
- [x] Correlate live runner observations against offline templates and CVE intelligence:
  `vulnerability_analysis.correlate_observations`, triggered explicitly via `POST /api/findings/correlate`
  (an operator/coordinator-initiated pass, not an eager per-job hook — evidence-producing paths are
  numerous and this keeps correlation itself an auditable, reviewable action). Deterministic only — no
  AI, no external CVE feed or CPE matching, nothing fetched live ("Vulnerability intelligence" is
  decided offline-only, see Product decisions above — this stays limited to CVE IDs already present
  in imported documents). An exact live host:port match against a host-bound imported
  finding transitions it to `confirmed-observed`; a port-number/service-name text match against an
  offline NASL template produces a new low-confidence (0.2–0.35) `candidate` finding for review — never
  silently promoted to confirmed. **Stated precision limit:** template matching is plain substring/number
  comparison against whatever free text got imported, nothing more; the confidence values reflect that.
- [x] Add safe validation state, suppressions, remediation tracking, and report exports:
  `open → candidate/confirmed-observed/confirmed/false_positive → remediated` as an explicit allowed-
  transition graph (`ALLOWED_STATUS_TRANSITIONS`, `set_finding_status`, rejects an invalid jump,
  audits every real transition); suppression (never deletes, only excluded from active counts,
  `set_finding_suppression`); remediation tracking (owner/due date/notes/status,
  `set_finding_remediation`); a signed `findings_bundle` export (`GET /api/findings/export`, same
  canonical-digest/HMAC pattern as `evidence_bundle`/`audit_bundle`) with severity/status counts.
  Re-importing an already-known finding now preserves its operator disposition instead of silently
  resetting it to `open` — a real bug this work found and fixed along the way. Web UI: a "CORRELATE NOW"
  action and a status dropdown + suppress toggle per finding card — web build and lint verified clean.

## Phase 5 evidence custody

- [x] Assign canonical SHA-256 content identities to new evidence records.
- [x] Chain project records by predecessor hash and sign each chain head with a local custody key.
- [x] Detect content, order, chain, and receipt tampering while preserving explicit legacy-record status.
- [x] Export signed JSON evidence bundles with verification results.
- [x] Encrypt desktop-node evidence spools at rest with per-record AES-256-GCM and node-bound AAD.
- [ ] Apply the authenticated encrypted-spool format to relay and ESP32 storage.
- [x] Carry provider signatures through ingestion rather than replacing them with coordinator-only
  custody: the outbox-pull path (`AutomationEngine._sync_outbox`) now carries the already-verified
  response signature from `evidence.outbox.read` forward as a `provenance` field on every evidence
  record it ingests, folded into the record's content hash rather than being discarded once the
  transport check passes. **Scope note:** this covers the outbox-pull path into the coordinator's own
  evidence ledger, which is where the gap was concretely identified. The separate push-in path
  (`storage.evidence.write` requests landing in `ReconclaveNode.write_evidence`'s AES-256-GCM spool,
  a different subsystem from the workspace-store ledger this covers) still only relies on outer
  request-auth without preserving it alongside the stored record — a narrower, separate follow-up if
  that spool's own provenance story needs the same treatment.

## Phase 6 operations timeline

- [x] Persist traceable audit events for scopes, jobs, workflow transitions, evidence, and imports.
- [x] Add a project-filtered, searchable live audit view.
- [x] Correlate node transport events into the same trace: `Coordinator.invoke()` accepts an optional
  `trace_id` and records the dispatch outcome (accepted/node_unavailable/unauthorized/rejected/
  transport_error/node_<status>) under it via an `audit_sink` callback, threaded through workflow-run
  step start/poll/cancel. Relay events remain N/A until Phase 8/11 relay nodes exist to emit them.
- [x] Add event pagination, retention controls, and signed audit export: `WorkspaceStore.list_audit_events`
  (event-id-anchored cursor, stable under concurrent appends), `prune_audit_events` (operator-triggered
  only, never automatic, self-auditing), and `audit_bundle` (signed with the custody key, same shape as
  `evidence_bundle`) exposed at `GET /api/audit`, `POST /api/audit/prune`, `GET /api/audit/export`.
- [x] Global cross-page notifications, derived from the audit trail rather than only from actions the
  browser itself initiated — this is what actually surfaces background/autonomous events (an automation
  rule firing, a recurring scan completing) that have no client-side call site to hook. A topbar bell
  (`web/src/App.tsx`) with an unread badge and dropdown is visible from every view, not just the Network
  page's existing embedded Activity Stream panel (both read the same underlying list). Added
  `automation.triggered` as a new audited moment (`AutomationEngine._trigger`) for desktop-evaluated
  rules; for rules pushed to a capable device (`device_managed: true`, the common P4 case), the rule
  fires entirely on-device and the desktop only learns about it via outbox-synced evidence carrying a
  `rule_id` — detected from that instead, since there is no separate trigger signal to audit. Scan
  started/complete/failed/cancelled reuses the existing `job.updated` audit event, de-duplicated against
  the Network view's own richer Scout-specific notifications via a shared last-notified-status map
  (`markJobNotified`) so a UI-driven scan doesn't produce two notifications for one event.

## Tool-runner safety contract

Tool support means explicit packaged adapters, not unrestricted shell access. Every adapter
must define its executable identity, argument schema, output parser, timeout, resource limits,
network scope behaviour, evidence schema, risk class, and cancellation semantics. Initial
safe adapters should target discovery and vulnerability analysis such as Nmap, Nuclei in
non-destructive template classes, testssl.sh, sslscan, Nikto, WhatWeb, DNS utilities, and
packet capture. Tools or flags that exploit, alter, persist on, evade, or disrupt a target
remain disabled unless a future approval policy explicitly permits that exact operation.

## Icebreaker reuse assessment

Candidate source areas under `/mnt/Storage/Coding/Icebreaker/`:

- `icebreaker/detectors/`: bounded network discovery and banner collection patterns.
- `icebreaker/analyzers/`: TLS, certificates, headers, DNS, API discovery, and disclosure checks.
- `icebreaker/core/cve_service.py`: CVE enrichment concepts and caching.
- `icebreaker/core/risk_scoring.py`: deterministic risk scoring.
- `icebreaker/importers/nasl_parser.py`: offline Nessus/NASL knowledge import.
- `icebreaker/writers/` and `icebreaker/reports/`: structured and human-readable exports.
- `icebreaker/agent_engine/policy.py`, `campaign.py`, and `controller.py`: policy and workflow
  lessons; these require review before reuse and are not copied automatically.

## Open design decisions

None currently open. The questions previously tracked here (relay transport, operator identity,
vulnerability intelligence sourcing, post-exploitation approval timing, evidence retention
governance, consensus-scanning phase placement, coordinator of record, non-IP scope delegation)
were all decided and moved into "Product decisions" above. Add new entries here as they come up;
a phase with a genuinely undecided architectural question shouldn't be scoped in detail until
it's resolved and recorded there.
