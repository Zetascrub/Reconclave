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

First generate the ignored trust headers from the repository root, using your
actual fleet identities (the following are synthetic examples):

```sh
python3 tools/provision_fleet.py --desktop-id rc-desktop-example \
  --p4-id rc-p4-example --cardputer-id rc-adv-example
```

See [fleet trust](../../docs/trust-architecture.md). Keep the store and generated
headers private. Built images contain deployment keys and are not public release
artifacts; see [release signing](../../docs/releasing.md).

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

The initial wire transport is bounded HTTP request/response. The provisioned
fleet build authenticates P4 requests and responses with a unique 256-bit
Cardputer-P4 key, a fresh per-request nonce, and the P4's per-boot challenge.
It identifies itself as the priority-50 secondary coordinator and requests a
short signed lease, allowing takeover when the priority-100 desktop is absent.
The legacy Grove/NVS pairing workflow remains available for recovery builds but
does not override generated fleet trust.

## CC1101 & NFC cap (M5Stack U219)

The **Zeta Mascot** theme is available under **Settings → Display → Theme**
(Left/Right selects a theme). Inspired by the mascot palette, it uses cyan
highlights, deep blue-grey panels, cream foreground text, tan secondary text
and amber accents. The selection persists across reboot; existing theme
preferences remain valid. The theme remaps shared UI colours without broadly
recolouring signal heat maps or changing error colours.

UI refinement: menus and Wi-Fi/BLE result lists show scroll position and keep
the final page populated. Long menu/context labels use pixel-fitted ellipses.
Footer hints show Tab only where the key is handled, and Wi-Fi/BLE export
failure messages remain visible. NFC editors retain their draft when returning
to the tools menu; cancelling write confirmation returns to editing. Editing
clears stale feedback, empty submissions explain what is needed, and a caret
marks the insertion point. Empty/invalid preset selections explain the issue.
The NFC viewer shows its visible line range. RF pause retains the last signal
and peak values, with explicit LIVE/PAUSED/ERROR status and matching Enter hints.

**Observe → NFC tag reader** starts continuous discovery with the cap's
ST25R3916. Hold one tag against the cap; Enter pauses/restarts scanning and Q
stops the RF field and returns. The scanner cycles through:

- NFC-A / ISO14443A: UID, ATQA, SAK and provisional type;
- NFC-B / ISO14443B: PUPI identifier (which may be randomized);
- NFC-F / FeliCa: IDm and PMm at both 212 and 424 kbit/s;
- NFC-V / ISO15693: UID and DSFID from inventory, without requiring the
  optional Get System Information command.

Up/Down browses the most recent 32 unique protocol/identifier pairs retained
until reboot. Tab → Save NFC evidence exports identifier, type hint, protocol
and details to microSD. The reader polls one protocol per loop pass, with a
field reset between passes to wake halted tags. SPI polling of interrupt status
avoids depending on GPIO interrupt delivery. Initialization errors distinguish
an absent chip from a chip that responds but fails RF setup.

These are the cap's four advertised NFC reader protocol families. Discovery
identifies tags; it does not promise access to their protected memory or identify
every manufacturer's model. 125 kHz RFID is different hardware and cannot be
read by this NFC cap.

**NFC tag reader → Tab → Write & emulate** provides text and HTTP(S) URL
NDEF messages, with up to 120 characters entered on the keyboard:

- **Write text / Write URL:** supports already NDEF-formatted NTAG213,
  NTAG215, NTAG216 and original MIFARE Ultralight tags. Enter checks the tag's
  format, advertised write permission and capacity, then shows its UID and
  replacement content. Keep that tag in place and press Enter again to write.
  The firmware checks the UID again and reads the NDEF message back to verify
  it. Q or Escape cancels the confirmation. Smaller tags accept shorter messages.
- **Emulate A text / URL:** presents a virtual NTAG213 Type 2 NDEF tag.
- **Emulate F text / URL:** presents a virtual FeliCa Lite-S Type 3 NDEF tag.
  Each session generates a fresh identifier. Content and reader writes live
  in RAM and are discarded when the session ends. Hold the cap near an NFC
  reader; Q, Escape or Enter stops emulation. Fleet servicing pauses while
  emulation is active to give NFC replies priority.

**NFC tag reader → Tab → Read text / URL content** reads the first NDEF record
from a Type 2 tag supported by the driver (NTAG2/MIFARE Ultralight families).
It displays the tag UID and UTF-8 text or a URI, expanding standard URI prefixes.
Up/Down scrolls; Enter reads the tag again; Q returns. Reading does not require
write access. Protected/unformatted tags, UTF-16, chunked or other record types
report a limitation rather than showing partial content. The viewer bounds
messages to 2048 bytes and the first record's payload to 1024 bytes; glyph
coverage depends on the display font. NFC-B/F/V discovery remains available,
but this content-viewing workflow is Type 2 only.

In any **write/emulate content editor**, press **Tab** to save or load presets
on microSD. There are 16 slots shared across projects; loading preserves the
chosen write/emulation action and selects text or URL to match the preset.
Review the full loaded content in the editor before pressing Enter. Saving
uses a free slot, checks the write and reads it back before renaming the
temporary file. Existing presets are never overwritten. The files are plain
text at `/reconclave/nfc-presets/1.txt` through `16.txt`: a leading `T` or `U`
followed by 1–120 printable ASCII characters, with no trailing newline. Manage
or delete these files from a computer to free slots. Invalid files are marked
and left untouched; an absent/unwritable card reports an error in the editor.

Writing replaces existing NDEF content; it does not format blank tags, change
keys or unlock protected memory. Physical lock bits or removing a tag during
writing can cause a partial write, which is reported on screen. Emulation
provides ordinary text/URL tags, not copies of access or payment credentials.
Native tests cover record encoding and emulated memory layouts; physical
write/readback and phone/reader interoperability still require device testing.

**Observe → Sub-GHz monitor** starts receive-only channel-energy monitoring
with the CC1101. Left/Right selects 315.00, 433.92, 868.00 or 915.00 MHz;
Enter stops/restarts reception; Q returns and stops the receiver. A scrolling
trace shows the latest 200 samples (20 seconds of reception at 10 Hz), with a
colour activity strip beneath it. New activity appears at the right, and a fixed
-120 to -30 dBm scale keeps bursts comparable. Each band has its own history;
pause freezes it and changing bands restores that band's trace. Last RSSI,
peak and cumulative sample counts are retained until reboot.

Up/Down (or `;` / `.`) adjusts the selected band's activity threshold in 5 dBm
steps from -120 to -30 dBm; the default is -80 dBm. The amber dashed line marks
that threshold, and `above` shows the percentage of retained samples at or above
it, with the window's sample count in parentheses. This is sample activity,
not packet counts or time-weighted channel occupancy. Each band retains its own
threshold until reboot. Pauses or sampling delays over 250 ms break the trace
line; the horizontal axis represents the last 200 samples, approximately 20
seconds during uninterrupted reception. Enter pauses the trace for inspection.
Summary and trace CSVs include the threshold and corresponding activity values
calculated at export time.

Tab → Save RF summary exports per-band summaries. Tab → Save RF graph samples
exports the selected band's retained timestamped samples.
Use the supplied antenna matching the selected band. This is signal activity
over time at one frequency, not a swept-frequency spectrum waterfall or
packet/remote decoder. RF transmission and replay are not implemented.

Both functions initialize on demand and report an unavailable cap instead of
preventing normal startup. No cap is required for Wi-Fi, BLE or other workflows.

The cap and microSD share SPI SCK=40, MOSI=14, MISO=39. CC1101 uses CS=5,
GDO0=15 and RF_SW0=13; NFC uses CS=6 and IRQ=4. Both cap chip selects are held
high before mounting microSD. NFC transactions use SPI mode 1; CC1101 uses
mode 0. RF_SW1 is the CC1101's GDO2 output: band selection uses both switch
lines, and asynchronous direct reception preserves GDO2's switch setting.
Refreshing microSD stops reception; restart it with Enter if necessary.

Implementation references:

- [M5Stack cap pinout and RF switch table](https://docs.m5stack.com/en/cap/Cap_CC1101)
- [M5Stack Arduino examples](https://docs.m5stack.com/en/arduino/projects/cap/cap_cc1101)
- [M5Unit-NFC driver](https://github.com/m5stack/M5Unit-NFC)
- [Evil-M5Project](https://github.com/7h30th3r0n3/Evil-M5project), reviewed for
  same-cap NFC/SPI support; application code was not copied.

### Hardware verification after flashing

1. Boot without the cap: verify Wi-Fi/BLE and evidence browsing still work;
   opening the cap tools should report unavailable hardware without hanging.
2. Power off, attach the cap and matching antenna, then boot. Hold a known
   NFC-A tag in range, verify its UID and that repeated detections occupy only
   one history entry. Repeat with NFC-B, FeliCa and ISO15693 tags.
   Read another tag and check Up/Down navigation. Retry with no tag present.
3. Export NFC observations, inspect the CSV in Evidence, refresh microSD and
   restart scanning to verify bus reuse and reader restart.
4. Open Sub-GHz monitor, check every band and compare readings with a known
   nearby signal source. Verify bursts leave a scrolling trace, Enter freezes/restarts it and Q stops RX.
5. Export RF summaries, inspect their counts/frequencies, then alternate NFC,
   RF monitoring and SD refresh. Verify Tab actions and back navigation, and
   confirm coordinator/network requests still respond during monitoring.

The firmware and native history/protocol tests build locally. Physical reads
with each tag family still require the actual tags and cap.

## Field usability and reliability update

- First boot: Escape skips Wi-Fi setup and opens offline field mode. From the
  connection screen, Q/Escape works offline and W opens Wi-Fi setup. Saved
  credentials are retained; Settings → Connectivity returns to setup.
- Wi-Fi and BLE discovery run in the background. Q returns and cancels the
  current scan; Tab also offers a stop action. Both scans are passive. Wi-Fi
  retains the strongest 128 APs; BLE retains at most 64 devices, and advertised
  names may be absent if they are only sent in a scan response. BLE refuses to
  interrupt an active network scan and restores Wi-Fi only if it was enabled
  before BLE started.
- NFC: Left/Right chooses Auto, NFC-A, NFC-B, FeliCa or ISO15693. The same
  control is in Tab → Protocol. Tab → Clear NFC history starts a fresh history.
- Sub-GHz: Tab → Save RF graph samples exports the selected band's retained
  samples with frequency, actual boot-relative sample time and RSSI. Tab →
  Clear this band's history resets its trace, peak and count. Acquisition can
  continue after clearing. Samples on either side of a pause remain adjacent
  on the screen; exported timestamps preserve the real time gap.
- CSV export names contain a random boot identifier and a sequence, and existing
  files are checked before opening. All operator CSV exports detect short writes
  and report storage failures. Scout CSV rows identify each actual observation
  provider; retained results without current provenance are labelled unknown.

Native regression checks:

```sh
cmake -S ../.. -B /tmp/reconclave-tests
cmake --build /tmp/reconclave-tests
ctest --test-dir /tmp/reconclave-tests --output-on-failure
```

After flashing, verify offline startup, cancellation of both discovery scans,
Wi-Fi restoration after BLE, protocol selection, RF trace export, and a full or
removed SD card's error handling. Native tests simulate driver failures and
exercise local TCP sockets; they do not replace these on-device checks.
