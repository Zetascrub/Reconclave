# Reconclave

**Development preview — source-first release preparation.** Hardware coverage
and production security work remain in progress. See [release signing](docs/releasing.md),
[security policy](SECURITY.md) and [contributing](CONTRIBUTING.md).

Reconclave is a platform for coordinating **authorised** security assessment
work across a desktop coordinator and a fleet of purpose-built ESP32 devices.
It is the successor to Ghostwire: the Cardputer ADV, Unit PoE-P4, and (once
available) a K230 vision node act as cooperating nodes rather than one
tightly coupled product.

> Only use Reconclave on systems and networks you own or are explicitly
> authorised to assess.

## Architecture at a glance

The desktop coordinator is the permanent hub: it holds signed engagement
scopes, dispatches workflows, and owns evidence custody. Devices are
capability-rich nodes, not dumb sensors — each announces itself over mDNS and
advertises the capabilities it supports (`system.info`, `net.discovery.scan`,
etc.). Protected coordinator-to-node requests use JSON envelopes authenticated
with per-peer HMAC keys and bound to a boot nonce to resist replay. Discovery
and transport encryption have different boundaries; see the security policy.
Nothing is master/slave: a K230 node, once added, is just another capable
node rather than a required brain for the fleet.

## Components

| Component | What it is |
|---|---|
| [`tools/desktop-node/`](tools/desktop-node/README.md) | The permanent coordinator: web UI, workflow engine, fleet management, evidence store |
| [`devices/poe-p4/`](devices/poe-p4/) | ESP-IDF (C) firmware for the M5Stack Unit PoE-P4 — an Ethernet-attached node |
| [`devices/cardputer-adv/`](devices/cardputer-adv/) | PlatformIO/Arduino (C++) firmware for the M5Stack Cardputer ADV — a handheld node/console |

## Requirements

- Python 3.11+ and Node 22 for the desktop coordinator and its web UI
- ESP-IDF v5.4.x to build/flash `devices/poe-p4/`
- PlatformIO to build/flash `devices/cardputer-adv/`
- CMake and a C++17 toolchain for the shared protocol library and its tests

## Features

- **Signed engagement scopes** — coordinator assessment workflows validate
  operator-authorised, expiring scopes; scope enforcement remains a security
  property to test across every execution path.
- **Workflow engine** — durable, retryable DAG workflows across nodes, with a
  drag-and-drop visual builder alongside the JSON form.
- **Adaptive distributed scheduling** — jobs are sharded and dispatched across
  the fleet by capability, load, and topology, with failover and consensus
  scanning across multiple vantage points.
- **Fleet management** — health/inventory tracking, config drift detection,
  and staged OTA rollout with rollback orchestration. Offline publisher
  signatures are available separately; hardware Secure Boot is not enabled.
- **Evidence pipeline** — content-addressed, chain-of-custody evidence with
  AES-256-GCM at-rest encryption on both the coordinator and supported devices.
- **Vulnerability analysis** — offline normalisation, correlation, and
  confidence scoring, including Nessus/NASL import.
- **Operators, roles, and approvals** — local accounts with maker-checker
  approval for sensitive actions.
- **Live operations timeline** — a searchable, correlated audit history across
  campaigns, jobs, nodes, and evidence.
- **Capability SDK** — packaged tool runners with signed manifests, schemas,
  and risk classes; availability is discovered per node, never assumed.

Implementation and validation vary by component. Native and desktop tests,
web builds and Cardputer compilation run in CI; they do not establish complete
hardware interoperability or a production security audit. K230, production
transport and hardware-enforced boot trust remain future work. See the
[roadmap](docs/platform-roadmap.md) and [firmware review](docs/cardputer-firmware-review.md)
for implementation notes and known limitations.

## How to use it

The quickest way to get the coordinator running:

```sh
./start-desktop.sh
```

This creates the Python virtualenv and installs web dependencies on first
run, rebuilds the web UI only when its source has changed, then starts
`desktop_app.py --mode both`. Any arguments are passed straight through, e.g.
`./start-desktop.sh --enable-network-scan --evidence-dir ./evidence`. Then
open <http://127.0.0.1:8767>. See the [desktop node guide](tools/desktop-node/README.md)
for the full set of options, including environment-based secrets.

Build and test the shared protocol library:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Before building or flashing a device, generate its private trust headers from
the repository root. Replace these synthetic IDs with your actual fleet IDs:

```sh
python3 tools/provision_fleet.py --desktop-id rc-desktop-example \
  --p4-id rc-p4-example --cardputer-id rc-adv-example
```

Follow the [trust guide](docs/trust-architecture.md) to obtain matching device
identities. Then follow the device's README/build instructions. The generated
headers and fleet store are private deployment material; do not commit them or
redistribute firmware compiled with them. Re-running with the same IDs preserves
keys; rotation is an intentional fleet-wide operation.

Devices announce themselves over mDNS (`_reconclave._tcp`) and appear
automatically in the coordinator's live roster.

## Further reading

- [docs/architecture.md](docs/architecture.md) — system architecture
- [docs/protocol.md](docs/protocol.md) — wire protocol
- [docs/trust-architecture.md](docs/trust-architecture.md) — fleet provisioning, identities, key rotation
- [docs/ui-design.md](docs/ui-design.md) — shared embedded interface style guide
- [docs/capabilities.md](docs/capabilities.md) — capability reference

## Public release status

Code and documentation are available under the [MIT licence](LICENSE), except
for third-party material under its own terms. The mascot artwork and generated
image data are **not MIT-licensed**; their rights are reserved under
[the artwork terms](ARTWORK_LICENSE.md). A fork that redistributes those assets
needs separate permission or replacement artwork.

The repository is being prepared for public viewing. See
[release preparation](docs/releasing.md) and [third-party notices](THIRD_PARTY_NOTICES.md).
