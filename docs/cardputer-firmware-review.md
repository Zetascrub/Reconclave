# Cardputer ADV firmware review — 7 September 2026

Reviewed the application navigation and startup flow, Wi-Fi/BLE/NFC/sub-GHz
observation paths, local host/port scan services, recurring scan restarts and
operator evidence exports. Existing in-progress cap support was preserved.

## Issues corrected

| Finding | Change |
| --- | --- |
| First boot had no working Escape exit without saved credentials; the connection screen advertised W without handling it. | Offline field mode and working connection-screen controls; SSIDs limited to 32 bytes. |
| Synchronous Wi-Fi and five-second BLE scans blocked keyboard and fleet servicing. | Background scan start/completion, cancellation, passive discovery and bounded retained results. |
| BLE unconditionally attempted Wi-Fi restoration, even for offline use, and could interrupt a local network scan. | Preserve prior Wi-Fi intent; reject BLE while network scans are active. |
| CSV names used only milliseconds since boot and could overwrite an earlier boot's evidence. | Random boot identifier, sequence and existence check. |
| Positive short SD writes could be reported as successful exports; the encrypted collector append ignored its write count. | Checked CSV stream and verified encrypted append length/error status. |
| Scout CSV assigned every host to one provider in distributed runs. | Per-provider rows; unknown provenance explicitly marked. |
| Port scanning ignored failure to enable nonblocking mode and ignored select errors. | Close failed setup sockets; consume readiness sets only after successful select. |
| Host scans could overwrite an active ping session or accept an empty range. Ping callbacks used volatile cross-task flags. | Reject conflicting/invalid starts; atomic callback state, session matching and start-failure cleanup. |
| Local recurring deadlines used a comparison that fails at millis rollover; failed restarts could discard prior results. | Wrap-safe deadline check and preserve results/deadline until restart succeeds. |

## Added field controls

- Select one NFC protocol or automatic A/B/F/V discovery.
- Clear NFC history or the current RF band's trace/peak/count.
- Export the actual RF graph samples with boot-relative timestamps.
- Show RF restart/stop errors instead of presenting stale RSSI as live data.

## Validation

- PlatformIO Cardputer ADV firmware build passed (48.6% flash, 24.4% static RAM).
- All four native test suites passed.
- Flashed connected ESP32-S3 (local test device); esptool verified the written
  image hash and reset the device.
- Shared protocol native tests.
- RF history tests: chronological wrap, independent histories, invalid values,
  graph clipping, timestamps and clearing.
- Scanner tests: rejected restart/empty range, skipped own address, late callback,
  ping start failure, /32 range rejection and real local open/closed TCP sockets.
- SD writer tests: silent positive short write, zero-byte write, persistent failure
  after later successful writes and final error status.

Physical NFC tag coverage, Wi-Fi/BLE cancellation and restoration, and full-card
behaviour still require on-device verification. The native scan tests use a mock
ping driver; they do not emulate ESP32 interrupt scheduling.

## NFC writing and emulation follow-up

Added text/URL NDEF writing for already formatted NTAG213/215/216 and original
MIFARE Ultralight tags. Preflight checks format, advertised write access and
capacity; the operator confirms the UID and replacement content, and the write
rechecks the UID before updating and verifying the NDEF message by readback.
Added fresh-identifier NTAG213/NFC-A and FeliCa Lite-S/NFC-F emulation using RAM
NDEF images. Emulation pauses fleet servicing and refuses to start during an
active scan so its polling loop can prioritize reader responses. Switching back
to discovery restores reader configuration explicitly.

The follow-up build passed at 50.3% flash and 24.6% static RAM. All five native
test suites passed, including NDEF record/TLV encoding, capacity rejection,
Type 2 UID/BCC and Type 3 attribute/checksum/memory layout checks. Physical tag
write/readback, emulation interoperability and repeated reader/emulator mode
switching still need on-device validation.
The follow-up image was flashed to ESP32-S3 (local test device); esptool verified
the written image hash and reset the device successfully.

## Remaining architectural work

### 8 September: RF activity controls

Added independent per-band thresholds (-120 to -30 dBm, 5 dBm steps), a dashed
graph marker and the percentage of retained samples meeting the threshold.
Summary exports append threshold/window/activity columns; trace exports append
threshold and per-sample classification. Existing leading CSV columns remain
in their original order. The graph no longer connects samples separated by more
than 250 ms, avoiding false continuity across pauses and slow foreground work.
The horizontal axis remains sample-based, not elapsed wall time.

PlatformIO build passed (50.3% flash, 24.6% static RAM) and all five native suites
passed, with new RF cases for threshold boundaries, window expiry, sampling gaps
and uptime rollover. Physical RF readings and screen controls need device testing.

### 8 September: NFC content viewer and presets

Added an operator-triggered Type 2 NDEF viewer displaying the UID and first
Text/URI record, with scrolling and retry controls. The decoder bounds TLV,
record, ID, language and payload lengths, expands standard URI abbreviations
and rejects chunked/UTF-16/unsupported records and control characters.
Added 16 microSD text/URL preset slots available from the content editor's Tab
menu. Loads validate file length, type and printable content; saves use new
slots with checked writes, readback and temporary-file rename. Loading returns
to the editor and never writes a tag or starts emulation automatically.

The firmware build passed (50.5% flash, 24.8% static RAM). All five native suites
passed, including new decoder tests for compressed URLs, long headers,
truncation, oversized lengths, language bounds and unsupported encodings.
Physical tag reads and SD preset round trips require on-device validation.
Flashed ESP32-S3 (local test device); esptool verified the image hash and reset
the device.

### Architectural follow-ups

The 8 September UI refinement adds bounded menu/context labels, scroll indicators
and stable final list pages, context-aware Tab badges, visible Wi-Fi/BLE export
feedback, preserved NFC drafts and return-to-editor cancellation. The NFC editor
has an insertion caret and explicit empty-input feedback; the viewer shows line
position. Invalid/empty preset selections now explain the issue. RF pause shows
retained last/peak readings, while receiver failures take priority over history.
Firmware build passed (50.6% flash, 24.8% RAM); all five existing native suites
passed. These tests do not validate the physical display or keyboard interaction.

Fleet discovery/HTTP requests and evidence delivery still use bounded synchronous
network calls. They can pause the UI when peers are unreachable; moving them to
an incremental network worker is a larger follow-up. Automatic multi-provider
change detection also shares a baseline and needs a dedicated provenance-aware
redesign; this update corrects operator CSV attribution only. Splitting the large
main.cpp by workflow would make these changes easier to test independently.
