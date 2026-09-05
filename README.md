# Reconclave

Reconclave is a platform for coordinating **authorised** security assessment
work across a desktop coordinator and a fleet of purpose-built ESP32 devices.
It is the successor to Ghostwire: the Cardputer ADV, Unit PoE-P4, and (once
available) a K230 vision node act as cooperating nodes rather than one
tightly coupled product.

> Only use Reconclave on systems and networks you own or are explicitly
> authorised to assess.

## Components

| Component | What it is |
|---|---|
| [`tools/desktop-node/`](tools/desktop-node/README.md) | The permanent coordinator: web UI, workflow engine, fleet management, evidence store |
| [`devices/poe-p4/`](devices/poe-p4/) | ESP-IDF firmware for the M5Stack Unit PoE-P4 — an Ethernet-attached node |
| [`devices/cardputer-adv/`](devices/cardputer-adv/) | PlatformIO/Arduino firmware for the M5Stack Cardputer ADV — a handheld node/console |

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

See [docs/platform-roadmap.md](docs/platform-roadmap.md) for the full delivery
roadmap and current phase status.

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
