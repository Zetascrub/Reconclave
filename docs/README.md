# Reconclave documentation

Start with the guide for the component you want to use. Each fleet is privately
provisioned: download source, generate your own trust keys, then build locally.

## Build and use

| Component | Guide |
|---|---|
| Reconclave Command | [Desktop coordinator](https://github.com/Zetascrub/Reconclave-Command) |
| FieldDeck | [Cardputer ADV field console](https://github.com/Zetascrub/FieldDeck) |
| Relay | [PoE-P4 wired node](https://github.com/Zetascrub/Relay) |
| Sightline | [K230 vision node](https://github.com/Zetascrub/Sightline) |
| ZetaDongle | [T-Dongle-S3 USB node](https://github.com/Zetascrub/ZetaDongle) |

## Understand the system

| Topic | Reference |
|---|---|
| Components and responsibilities | [Architecture](architecture.md) |
| Requests and messages | [Protocol](protocol.md) |
| Available operations | [Capabilities](capabilities.md) |
| Fleet identities and private keys | [Trust architecture](trust-architecture.md) |
| Cross-device identity and visual system | [Product style guide](style-guide.md) |
| Embedded interface conventions | [Embedded UI design](ui-design.md) |
| Brand identity and scope direction | [Identity & scope review](identity-and-scope-review.md) |
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
