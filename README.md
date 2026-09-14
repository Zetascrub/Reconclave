<div align="center">

# Reconclave

**The shared protocol, trust model, and design system for a cooperating family
of standalone security-assessment devices.**

[Protocol](docs/protocol.md) · [Architecture](docs/architecture.md) ·
[Style guide](docs/style-guide.md) · [Trust](docs/trust-architecture.md)

</div>

Reconclave is the umbrella project. It defines how independently useful devices
discover one another, describe capabilities, exchange bounded requests, retain
evidence, and present a consistent product identity. Device firmware and the
desktop coordinator live in focused repositories so each can evolve and ship on
its own hardware cadence.

> Use Reconclave and its devices only on systems, networks, and radio
> environments you own or are explicitly authorised to assess.

## Product family

| Project | Hardware / role | Standalone role | Reconclave role |
|---|---|---|---|
| [Reconclave Command](https://github.com/Zetascrub/Reconclave-Command) | Desktop coordinator | Projects, workflows, local tools, and evidence review | Preferred fleet coordinator |
| [FieldDeck](https://github.com/Zetascrub/FieldDeck) | M5Stack Cardputer ADV | Portable field console and signal toolkit | Mobile coordinator and observation node |
| [Relay](https://github.com/Zetascrub/Relay) | M5Stack Unit PoE-P4 | Wired diagnostics and local API | Stable Ethernet execution node |
| [Sightline](https://github.com/Zetascrub/Sightline) | LILYGO T-Display K230 | Visual capture and edge-analysis instrument | Vision, positioning, and analysis node |
| [ZetaDongle](https://github.com/Zetascrub/ZetaDongle) | LILYGO T-Dongle-S3 | Physical USB payload, storage, and loot tool | Portable USB automation node |

Zeta is the shared mascot; Reconclave is the product family and protocol.
Device names describe their purpose without preventing them from working alone.

## What remains canonical here

- `protocol/` contains JSON schemas and interoperability test vectors.
- `common/protocol/` contains the transport-independent C++ domain library.
- `common/identity/` contains generated palette and identity tokens consumed by
  device repositories as versioned snapshots.
- `docs/` defines architecture, capabilities, trust boundaries, UI conventions,
  release expectations, and the product style guide.
- `tools/` validates protocol artifacts and supports source/release operations.

Device repositories vendor the small shared sources they compile against. A
device adopts protocol or identity changes deliberately, allowing compatibility
to be reviewed rather than changing every firmware build implicitly.

## Protocol principles

- Capability-driven coordination: nodes announce what they actually support.
- Standalone-first operation: loss of the coordinator does not make a device
  useless.
- Discovery is not authentication: protected operations require provisioned
  trust and an operator-authorised, bounded scope.
- Evidence retains source, time, operation, and custody context.
- Coordinators use priorities and expiring leases to avoid ambiguous ownership.
- Unsupported or unverifiable work fails closed.

See the [wire protocol](docs/protocol.md), [capability reference](docs/capabilities.md),
and [trust architecture](docs/trust-architecture.md) for normative details.

## Validate the shared contract

```sh
cmake -S . -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure
python3 tools/protocol-debugger/validate_vectors.py protocol/test-vectors/*.json
```

Each device repository documents its own toolchain, build, flash, and hardware
validation steps.

## Licensing

Code and documentation are MIT licensed unless a bundled dependency says
otherwise. Zeta mascot artwork and generated image data are governed by
[ARTWORK_LICENSE.md](ARTWORK_LICENSE.md), not the MIT licence.
