# K230 field toolset architecture

The K230 firmware is an assessment appliance, not a collection of unrelated
payload buttons. Every tool is represented by one capability contract used by
the local UI and Reconclave remote dispatch.

## Operating states

- **Observe**: receive/read-only collection that emits no assessment traffic.
  It can run without a scope but sensitive evidence still carries project,
  operator, time and location metadata.
- **Assess**: bounded discovery and protocol interrogation. It requires an
  active signed scope or a provisioned standing scope grant.
- **Armed**: disruptive, credential-handling or transmit-heavy actions. It
  requires an explicit per-action confirmation in addition to scope and is
  never enabled by boot, schedule or proximity alone.

The current mode must remain visible in the status bar. Starting a tool creates
a cancellable job; leaving its screen must not silently stop or detach it.

## Tool families

| Family | First capabilities | K230 advantage |
|---|---|---|
| Wireless survey | `radio.wifi.scan`, BLE discovery | GNSS-tagged surveys and persistent AP history |
| Network discovery | ARP snapshot, bounded host discovery, TCP service checks, DNS | Can operate locally or as a Reconclave scan shard |
| Vision | Camera capture, OCR, QR/barcode decoding, equipment-label capture | K230 camera and KPU acceleration |
| Radio | LoRa receive/spectrum observations, scoped transmit tests | SX1262/LR2021 plus location-tagged RF evidence |
| Cellular/GNSS | Position, cellular registration and signal observations | nRF9151 keyboard-base modem |
| Evidence | Timeline, notes, screenshots, PCAP/media manifests, export/outbox | One case record across every sensor and remote job |
| Coordination | Node discovery, job queue, node/coordinator role, workflow launch | Reconclave distributed execution rather than isolated gadgets |
| Hardware | Keyboard shortcuts, battery/thermal/environment health | Reliable field operation and graceful low-power capture shutdown |

## Workflow model

Borrow the useful product pattern of autonomous action queues, but bind each
mission to an assessment record and scope:

1. Select or create an assessment.
2. Import/verify a signed scope or choose Observe-only mode.
3. Run a preset such as **Site survey**, **LAN inventory**, **Camera/OCR notes**
   or **Remote sensor post**.
4. Watch jobs, findings and sensor health in one mission timeline.
5. Export a manifest with hashes, timestamps, node identity, scope revision and
   optional GNSS fix.

## Near-term implementation order

1. Finish nRF9151 power control and validate GNSS on hardware.
2. Integrate the TCA8418 mapping with LVGL navigation and text input.
3. Execution-key verification, replay protection, signed delegated scope,
   canonical JSON, SHA-256/HMAC and strict trust-key loading are implemented.
4. Bounded TCP connect, DNS, Wi-Fi, `/24` discovery and conservative service
   identification are remotely callable. Identification uses server-first
   banners or a fixed HTTP HEAD request and never attempts authentication.
5. The append-only mission timeline now hash-chains authenticated tool results
   with assessment context and verifies integrity in the UI. Build the durable
   per-host knowledge base now tracks services and state changes, with a
   deduplicated evidence-derived findings view.
6. Evidence manifests verify the timeline before publication, hash regular
   evidence files with SHA-256, bind assessment and firmware identity, exclude
   links, and enforce file-count and per-file limits for predictable field use.
6. Add protected camera capture, OCR and QR/barcode evidence.
7. Add receive-first LoRa and BLE surveys; put all transmit actions behind the
   Armed state.
8. Add coordinator discovery, job dispatch, progress and evidence outbox sync.

## Reference patterns

- Bjorn: autonomous queue, accumulated host knowledge and glanceable progress.
- Bruce / Evil-Cardputer: fast category navigation, persistent settings and
  hardware-aware feature availability.
- Hak5 devices: portable repeatable workflows and simple field activation.
- Ragnar: protocol/service-oriented modules and reusable tool interfaces.
- Raspyjack: compact headless deployment and remote-first control.

Do not copy unsafe defaults: no autonomous brute force, credential collection,
deauthentication, destructive payload, or unrestricted target input. Those
behaviours conflict with Reconclave's signed-scope and evidence model.
