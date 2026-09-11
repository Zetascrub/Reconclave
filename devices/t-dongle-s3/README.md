# LILYGO T-Dongle-S3

A new Reconclave fleet device: LILYGO's T-Dongle-S3, a USB-A-plug-form-factor
ESP32-S3 board. Chosen role: USB HID / BadUSB-style tool, gated by the same
signed-scope/trust model already implemented for the K230's network
capabilities (see `devices/k230`'s `trust_policy.cpp`) — advertised as a
capability but never executing HID injection without a valid signed scope
and evidence logging. No autonomous or silent-autorun payload behavior is a
project constraint (see `FIELD_TOOLSET.md`), and it applies here just as it
does to every other tool in this fleet.

This device is used only against hardware the operator owns and has
authorization to test, on a local lab network.

## Hardware, confirmed against the real unit (not assumed from docs)

Public spec sheets for this board disagree on some details (some say 4MB
flash, others 8MB). Rather than trust any of them, every fact below was
verified directly against the physical unit:

- **Chip**: genuine ESP32-S3, QFN56, revision v0.2 — via `esptool chip-id`.
- **Flash**: 16MB — via `esptool flash-id`.
- **USB**: native USB-OTG (TinyUSB), not the separate "Hardware CDC and
  JTAG" peripheral — required for `USBHIDKeyboard`/`USB.h`. Needs
  `ARDUINO_USB_MODE=0` + `ARDUINO_USB_CDC_ON_BOOT=1` at build time (same
  flags `devices/cardputer-adv` already uses for the same reason).
- **RGB LED**: APA102 (separate data+clock lines), NOT WS2812 — data on
  GPIO40, clock on GPIO39. Confirmed from LILYGO's own
  `Xinyuan-LilyGO/T-Dongle-S3` `examples/led/led.ino`, not from third-party
  pinout pages that describe it as a plain single-wire "RGB LED".
- **LCD**: ST7735 80x160, MOSI=3 CLK=5 CS=4 DC=2 RST=1, backlight on
  GPIO38 (active-low: 0=on, 1=off).
- **SD card**: SD_MMC 4-bit mode, CLK=12 CMD=16 D0=14 D1=17 D2=21 D3=18.
- **Button**: GPIO0 — the universal ESP32 boot-strap pin. LILYGO's own
  `examples/usb_hid_keyboard.ino` references it via a `BOOT_PIN` macro, but
  that macro lives in a board-variant header that doesn't exist for
  PlatformIO's generic `esp32-s3-devkitc-1` board (which this project builds
  against, since no PlatformIO board entry exists for this hardware — same
  approach `devices/cardputer-adv` takes for its own unlisted M5Stack
  board). GPIO0 is hardcoded instead of depending on that macro.

## Toolchain

`platformio.ini` uses `platform = espressif32@6.7.0`,
`board = esp32-s3-devkitc-1` (generic stand-in board, flash size and
partition table overridden to match the real 16MB chip via
`board_upload.flash_size` / `board_build.partitions = default_16MB.csv`),
`framework = arduino`, and `lib_extra_dirs = ../../common` for shared
Reconclave code.

## Milestone 1: toolchain + basic I/O proof (done, verified on hardware)

`src/main.cpp` is deliberately minimal: no USB HID, no LCD, no network yet.
It only proves the build/upload pipeline works and that the APA102 LED and
the GPIO0 button respond the way LILYGO's own examples say they should.

Verification against the real unit:

- Compiled clean: RAM 9.5% (327680B budget), Flash 4.7% (6553600B budget).
- `pio run -t upload` wrote and hash-verified 308768 bytes at 0x10000.
  The upload's final auto-reset-via-RTS step threw
  `OSError: [Errno 71] Protocol error` — this is a known, cosmetic
  ESP32-S3-native-USB-CDC quirk (the port handle drops as the chip resets
  and the USB device re-enumerates) and does not indicate the flash write
  failed; the write+verify had already succeeded before that step ran.
- Confirmed the new firmware was genuinely running two ways that don't
  depend on winning the race against the app's one-shot boot banner over
  serial (`Serial` only prints once at startup, and CDC has no host-side
  buffering — a listener that opens even slightly late misses it):
  - **LED**: visually confirmed blinking blue (~2Hz), matching the
    firmware's idle blink loop.
  - **Button**: visually confirmed the LED turns solid red while the
    button (GPIO0) is held, matching the firmware's button-pressed branch.
- Serial console itself was not confirmed working live in this pass — every
  attempt to catch the boot banner or a button-triggered log line lost the
  race against the one-shot print. Not a known defect; if live serial debug
  logging is needed for a later milestone, add a repeating heartbeat line
  in `loop()` so a listener has something to catch regardless of timing.

## Next steps (not started)

Milestone 2+: USB HID descriptor scaffolding (enumerate as a keyboard, no
injection logic yet), then wiring actual keystroke-injection capability
behind Reconclave's signed-scope trust model — advertised, logged, never
autonomous.
