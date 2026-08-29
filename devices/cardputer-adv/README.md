# Cardputer ADV firmware

The v0.1 firmware is both a Reconclave node and coordinator. It provides:

- keyboard-based first-run Wi-Fi provisioning with credentials stored in NVS;
- `_reconclave._tcp` discovery and advertisement;
- a multi-node Reconclave roster containing the local Cardputer and discovered
  Reconclave nodes, with per-node management pages and 45-second remote expiry;
- remote `system.info` requests to the P4;
- a local `system.info` handler for requests from future coordinators;
- a mission-oriented home screen derived from Ghostwire's interaction model;
- passive Wi-Fi discovery with SSID, channel, RSSI, and security state;
- a Wi-Fi channel analyser that scores the standard 2.4 GHz choices and
  recommends the least-congested of channels 1, 6, and 11;
- passive BLE discovery with address, RSSI, and connectability, including
  address/advertisement types, service UUID summary, payload size, and safe
  restoration of Wi-Fi after the shared radio is released;
- detailed Wi-Fi and BLE observation views with operator-triggered CSV export;
- a `/reconclave/evidence` microSD browser with bounded previews for text,
  CSV, log, JSON, JSON Lines, and Markdown evidence;
- an on-device system, storage, network, and trust diagnostics screen;
- a Field Kit hierarchy with a live network dashboard for SSID, signal, IP,
  gateway, subnet, DNS, and MAC details;
- persistent Display & Interface controls for Field, Night City, and Amber
  themes; Cards/List home navigation; brightness; screen timeout; Radar/Nodes
  idle animation (or display sleep); optional themed boot sequences with preview
  and speed control; and opt-in node discovery at boot (disabled by default);
- a structured Settings hierarchy for connectivity, display, storage/evidence,
  device information, and trust. Trust removal is local-only, explicitly
  confirmed, and warns that the P4 trust record must also be reset before a
  fresh Grove pairing;
- a responsive, capability-driven Scout workflow with Single, Distributed, and
  Auto execution. Live providers are discovered dynamically, address ranges are
  allocated without overlap using equal or weighted chunks, and results are
  merged with their observation vantage when exported as evidence;
- the portable shared protocol/node-registry library.

## Build

```sh
cd devices/cardputer-adv
pio run
```

## Flash

Confirm the serial port before writing, then use:

```sh
pio run --target upload --upload-port /dev/ttyACM0
pio device monitor --port /dev/ttyACM0 --baud 115200
```

On first boot, type the Wi-Fi SSID and password using the keyboard. The home
screen uses `W`/`S` to move, `Enter` to open, and `Q`, Escape, or Backspace to
return. Open **Reconclave** to discover the P4 with `R`, request authenticated
system information with `Enter`, or establish persistent trust with `P` while
the Grove cable is attached. Open **Observe signals** for Wi-Fi discovery,
**Evidence** for the microSD evidence store, and **Field kit** for diagnostics.
Wi-Fi configuration is available under **Settings**.

In **Scout**, use Left/Right to choose `Auto`, `Local`, or `P4`, then press `R`
to run. `Auto` uses the paired P4 when available and otherwise scans from the
Cardputer. Press `Tab` after completion to save the results to microSD. Local
discovery uses ICMP from the Cardputer's Wi-Fi vantage; the P4's wired discovery
also uses its Ethernet neighbor data, so the two perspectives can legitimately
produce different host counts.

Navigation follows one rule throughout the application: Up/Down moves through
lists, Left/Right changes horizontal choices, Enter opens or confirms, and
`Q`, Escape, or Backspace returns to the parent screen. The Cardputer's
`; , . /` keys mirror Up/Left/Down/Right outside text-entry screens. Back
navigation restores the relevant parent selection rather than jumping to the
first menu item.

Select a discovered host in **Scout** and press Enter to open its host card.
Enter again performs Ghostwire's bounded, non-blocking check of 13 common TCP
services. This intentionally avoids a full 65,535-port sweep in the normal
field workflow.

`Tab` consistently opens the contextual options menu for the current area.
Scout exposes execution target, service-scan scope (`Web 6`, `Common 13`, or
`Extended 39`), and evidence export. Wi-Fi and BLE expose rescan and evidence
actions; Reconclave exposes discovery and Grove pairing; Evidence exposes SD
refresh. Tab, `Q`, or Escape closes the menu and restores the exact underlying
screen and selection.

The **Reconclave** screen is the node-management parent. Up/Down selects the
local Cardputer or any discovered remote node and Enter opens that node's
management page. Its Tab menu contains roster-wide discovery and Grove-pairing
actions. Announcements are keyed by device ID, so future K230, Cardputer, and
P4 nodes can coexist in the roster; capabilities that are not implemented for
a particular node remain unavailable instead of being treated as P4 actions.

The initial wire transport is bounded HTTP request/response and exposes only
non-sensitive `system.info`. The paired build authenticates requests and responses with a
persistent 256-bit Grove-provisioned secret, a fresh per-request nonce, and a
per-boot P4 challenge. Pairing survives power loss because both peers store the
trust record in NVS.
