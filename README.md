# Reconclave

Reconclave is a capability-driven platform for coordinating authorised
security assessment work across heterogeneous embedded devices. It is the
successor to Ghostwire, with the Cardputer ADV, Unit PoE-P4, and T-Display
K230 treated as cooperating nodes rather than one tightly coupled product.

This repository currently contains the v0.1 protocol proof foundation:

- a portable C++17 protocol/domain library suitable for ESP-IDF and Arduino;
- strict validation for protocol envelopes, node announcements, requests,
  responses, and events;
- a capability-aware node registry with expiry handling;
- JSON Schemas and example protocol messages;
- native tests requiring no hardware or third-party dependencies.

## Build and test

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Hardware status

| Target | Current status | Planned first integration |
|---|---|---|
| Cardputer ADV | Firmware builds | Node/coordinator, discovery UI, `system.info` client/server |
| Unit PoE-P4 | Firmware builds | Ethernet node, mDNS, `system.info` server |
| Desktop/laptop | Python script available | Read-only mDNS node and `system.info` server |
| T-Display K230 | Awaiting hardware | Capability-aware console and vision capabilities |

The original Ghostwire repository remains unchanged. Device-specific code will
be migrated deliberately behind capability handlers instead of copied wholesale.

See [docs/architecture.md](docs/architecture.md) and
[docs/protocol.md](docs/protocol.md) for the initial implementation contract.
The [embedded interface style guide](docs/ui-design.md) defines the shared
visual, navigation, layout, contextual-menu, and copy rules for Cardputer and
the future K230 interface.
The [pre-K230 implementation plan](docs/pre-k230-plan.md) defines the next
Cardputer/P4 vertical slice and the security gate before remote assessment.

Only use Reconclave on systems and networks you own or are explicitly
authorised to assess.
