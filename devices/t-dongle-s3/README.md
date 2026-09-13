# T-Dongle-S3 — Reconclave fleet node

A trust-governed Reconclave node on the LILYGO T-Dongle-S3 (ESP32-S3). It is a
capability node like the PoE-P4 or Cardputer: it joins the fleet network, speaks
`reconclave/1`, and exposes **HID keystroke injection** as a capability the
coordinator can invoke — but only under the fleet's authenticated, scope-gated,
evidence-logged trust model.

> **This is not the standalone ZetaDongle.** The offline, button-only,
> Malduino-W-style tool was extracted to its own project
> (`/mnt/Storage/Coding/ZetaDongle`). Mass storage (MSC) exfiltration lives
> **there**, not here — inside the fleet it would need the deferred
> post-exploitation approval policy. Same hardware, two firmwares, two purposes.

## Architecture

A capability-module framework (the same shape as ZetaDongle's, ported): an `App`
core owns shared services and runs self-registering modules.

- **Services:** `UsbManager` (HID keyboard), `RadioManager` (STA join + mDNS
  `_reconclave._tcp`), `NodeService` (HTTP `announce`/`message`, protocol,
  dispatch), `Trust` (provisioned HMAC), `StatusLed`, `Config` (NVS),
  `InputService`, `TriggerPolicy`.
- **`TriggerPolicy` is the chokepoint.** A network trigger is authorised only if
  the request is **authenticated (HMAC)** *and* a **signed engagement scope is
  verified**.
- **Modules:** `input.hid.keystroke` (run a DuckyScript payload on the keyboard).

## Trust (interop with the fleet)

Request verification mirrors the PoE-P4 and the desktop coordinator exactly:

```
digest    = sha256( canonical_json(arguments) )                          (hex)
canonical = source|dest|request_id|capability|boot_nonce|digest|nonce|priority|lease_ms
tag       = HMAC-SHA256(peer_key, canonical)[:16]                        (hex)
```

`canonical_json` is Python's `json.dumps(x, sort_keys=True, separators=(",",":"))`,
reimplemented on-device. Each boot generates a fresh 16-byte nonce; request
nonces are tracked for replay resistance.

## Status — milestone 1 (compiles clean, not yet bench-verified)

- [x] Framework, services, protocol announce, mDNS/STA, HTTP endpoints.
- [x] Provisioned-HMAC request authentication (the format above).
- [x] `input.hid.keystroke` registered and **fail-closed**: authenticated
  requests are still rejected `SCOPE_REQUIRED` because signed-scope verification
  isn't implemented yet, so the node **cannot type unscoped**. The execution
  path (DuckyScript on the keyboard) is wired and compiled, gated only by that
  check.

### Milestone 2 (not started)

- [ ] Verify the argument-bound signed **scope-delegation** token (as
  `net.discovery.scan` does), flipping `scope_verified` true and enabling HID.
- [ ] **Evidence**: log each fire as an encrypted evidence record (RC_STORAGE_KEY).
- [ ] Sign responses (coordinator-side response verification).
- [ ] Coordinator **lease/priority** handling.
- [ ] Extend `tools/provision_fleet.py` with a `--tdongle-id` flag to generate a
  real `src/generated_trust.h` (currently an all-zero example for compilation;
  gitignored like the other device trust headers). A CI build job depends on this.
- [ ] Wi-Fi credential setup flow (today: NVS keys `wifi_ssid`/`wifi_pass`).

## Build

```sh
cd devices/t-dongle-s3
pio run          # RAM ~18%, Flash ~13%
```

Flashing uses the same no-auto-reset dance as the standalone firmware (hold the
button, replug to enter the ROM bootloader; plain replug after upload to boot).
