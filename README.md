# Reconclave

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
etc.). All coordinator↔node traffic is a JSON envelope, HMAC-authenticated
with per-peer provisioned keys and bound to a boot nonce to resist replay.
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

- **Signed engagement scopes** — every target-bearing action is bound to an
  operator-authorised, expiring scope; nothing runs outside it.
- **Workflow engine** — durable, retryable DAG workflows across nodes, with a
  drag-and-drop visual builder alongside the JSON form.
- **Adaptive distributed scheduling** — jobs are sharded and dispatched across
  the fleet by capability, load, and topology, with failover and consensus
  scanning across multiple vantage points.
- **Fleet management** — health/inventory tracking, config drift detection,
  and staged, signed OTA rollout with automatic rollback.
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

Most of the platform above is complete and running on real hardware today.
Still on the roadmap: secure relay/gateway nodes for VPN and internet-relayed
deployments, the rest of Phase 10 (notes, report exports, ATT&CK/STIX/OCSF
mappings), and a K230 edge-AI tier, which is gated on that hardware arriving.
See [docs/platform-roadmap.md](docs/platform-roadmap.md) for the full delivery
roadmap and current phase-by-phase status.

## How to use it

Build and test the shared protocol library:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Run the desktop coordinator (see the [desktop node guide](tools/desktop-node/README.md)
for the full setup, including environment-based secrets and network-scan options):

```sh
cd tools/desktop-node/web && npm install && npm run build && cd ..
../../.venv-desktop-node/bin/python desktop_app.py --mode both
```

Then open <http://127.0.0.1:8767>.

Flash a device by following its own README/build instructions under
[`devices/poe-p4/`](devices/poe-p4/) or [`devices/cardputer-adv/`](devices/cardputer-adv/),
then provision trust material for the whole fleet with:

```sh
python3 tools/provision_fleet.py --desktop-id <id> --p4-id <id> --cardputer-id <id>
```

Devices announce themselves over mDNS (`_reconclave._tcp`) and appear
automatically in the coordinator's live roster.

## Further reading

- [docs/architecture.md](docs/architecture.md) — system architecture
- [docs/protocol.md](docs/protocol.md) — wire protocol
- [docs/trust-architecture.md](docs/trust-architecture.md) — fleet provisioning, identities, key rotation
- [docs/ui-design.md](docs/ui-design.md) — shared embedded interface style guide
- [docs/capabilities.md](docs/capabilities.md) — capability reference
