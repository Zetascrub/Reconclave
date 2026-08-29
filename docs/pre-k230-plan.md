# Pre-K230 implementation plan

The K230 is not on the critical path for proving Reconclave. The Cardputer ADV
and Unit PoE-P4 can establish the complete v0.1 vertical slice first.

## Recommended transport profile

Use the P4's working Ethernet foundation from Ghostwire as the first adapter:

- mDNS service: `_reconclave._tcp.local`;
- one WebSocket connection for announcements, requests, responses, and events;
- JSON text frames, with one complete protocol message per frame;
- HTTP only for local health/debug endpoints, not as a second command protocol;
- a 16 KiB message limit and bounded connection/message queues.

Keeping one command/event channel prevents the HTTP, WebSocket, and Grove
contracts from drifting. Grove UART can later carry the same JSON envelope
using length-prefix framing, or a compact encoding, without changing domain
messages.

## Milestone A: local dispatch

On each device, introduce a small capability dispatcher mapping a capability
identifier to a handler. Route `system.info` through it locally and verify that
unknown capabilities produce `rejected/CAPABILITY_UNAVAILABLE`. This proves the
abstraction without any networking.

## Milestone B: P4 node

Migrate only the known-good Ghostwire P4 foundations: Ethernet initialisation,
mDNS, WebSocket lifecycle, and status LED. Rename its service identity, emit a
periodic announcement, and expose only `system.info`. Do not migrate remote
scan or payload endpoints yet.

## Milestone C: Cardputer coordinator

Reuse the Cardputer's Wi-Fi and display/input foundations. Discover the P4 via
mDNS, maintain `NodeRegistry`, show live nodes and capabilities, and issue a
`system.info` request. A vanished P4 should expire cleanly from the display.

## Milestone D: trust bootstrap

Before enabling assessment capabilities, add explicit physical pairing,
authenticated session establishment, integrity protection, replay windows, and
persisted peer identity. A TLS-protected transport does not replace node-level
permissions or scope enforcement.

## Milestone E: first scoped job

Add an engagement object with allowlisted CIDRs and prohibited targets. The P4
must independently validate it. Then expose a low-impact host discovery job
with accepted response, progress/evidence events, completion, cancellation,
timeouts, and an audit record.

## Decisions to settle before K230 integration

1. Device identity provisioning and recovery after a lost/replaced node.
2. Whether wall-clock time is mandatory before RTC/network time is available;
   sequence numbers and monotonic uptime must still work during cold start.
3. Maximum node count, concurrent jobs, evidence size, and stream timeout for
   each hardware tier.
4. Evidence retention and deletion policy per engagement.
5. A stable capability versioning rule when request/result shapes evolve.

The K230 can then join as another coordinator/node using an already exercised
protocol instead of becoming the place where transport, orchestration, UI, and
device discovery are debugged simultaneously.
