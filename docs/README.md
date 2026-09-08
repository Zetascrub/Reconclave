# Reconclave documentation

Start with the guide for the component you want to use. Each fleet is privately
provisioned: download source, generate your own trust keys, then build locally.

## Build and use

| Component | Guide |
|---|---|
| Desktop coordinator | [Setup and workflows](../tools/desktop-node/README.md) |
| Cardputer ADV | [Build, flash and field tools](../devices/cardputer-adv/README.md) |
| Unit PoE-P4 | [Build, flash and Ethernet node](../devices/poe-p4/README.md) |
| K230 | [Planned hardware](../devices/k230/README.md) |

## Understand the system

| Topic | Reference |
|---|---|
| Components and responsibilities | [Architecture](architecture.md) |
| Requests and messages | [Protocol](protocol.md) |
| Available operations | [Capabilities](capabilities.md) |
| Fleet identities and private keys | [Trust architecture](trust-architecture.md) |
| Firmware interface conventions | [UI design](ui-design.md) |
| Reliability and recovery | [Reliability](reliability.md) |

## Develop and release

- [Contributing and checks](../CONTRIBUTING.md)
- [Source releases and publisher signatures](releasing.md)
- [Public release checklist](public-release-readiness.md)
- [Security policy and reporting](../SECURITY.md)
- [Roadmap](platform-roadmap.md)
- [Cardputer implementation and validation notes](cardputer-firmware-review.md)
- [Cardputer/P4 validation procedure](cardputer-p4-validation.md)

The [original design document](../Reconclave_Design_Document_v0.1.md) records
architecture history and planned concepts. It is not a promise that every
feature described there is implemented. Device guides and the roadmap carry
current implementation details; physical validation limitations remain explicit.
