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

## Milestones 2+3: USB HID keyboard + ST7735 display (done, verified on hardware)

Both landed together since they shared a debugging session. `src/main.cpp`
now also enumerates as a composite USB HID keyboard and shows a boot logo
on the ST7735.

### USB HID keyboard, physical-button-triggered only

Uses Arduino-ESP32's `USBHIDKeyboard` (from the `USB`/`USBHIDKeyboard`
libraries bundled with `framework-arduinoespressif32`, confirmed present
and `CONFIG_TINYUSB_HID_ENABLED=1` in this framework version's sdkconfig
before relying on it). The only trigger is the device's own physical
button — there's no network stack yet, so there is no remote/autonomous
path to a keystroke at all right now. That's deliberately conservative,
not the final design: once this device gains network connectivity, actual
remote-triggered HID injection must be gated by the same signed-scope/
trust model `devices/k230`'s `trust_policy.cpp` already implements
(verified scope + evidence logging), never a bare "message arrives, keys
get typed" path. The test payload is deliberately inert too — no Enter,
no modifier keys, plain text — so it can't execute a focused shell command
or trigger a shortcut, only visibly type a marker string.

Verified on hardware: `lsusb -v` shows a genuine composite device
(`bInterfaceClass 3` Human Interface Device + Communications/CDC Data),
and pressing the button three times in a row typed
`reconclave-t-dongle-s3 hid-test-ok #1/#2/#3` into a focused text field,
counter incrementing correctly each time.

One cosmetic gotcha hit along the way: `USB.productName()`/
`manufacturerName()` calls in `setup()` silently no-op under
`ARDUINO_USB_CDC_ON_BOOT=1`, because that flag makes the core call
`USB.begin()` automatically *before* `setup()` runs (so `Serial` works
immediately) — by the time our code could call those setters, `_started`
is already true and they're guarded no-ops. Fixed by setting
`USB_MANUFACTURER`/`USB_PRODUCT` as compile-time build flags instead,
since those become the `ESPUSB` constructor's default values before any
runtime code executes.

Also note: `USBHIDKeyboard` sends US-layout HID keycodes regardless of the
host's actual keyboard layout — confirmed on a UK-layout host, where the
`#` in the test string rendered as `£` (the UK-layout character at that
same physical key position). This is expected, not a bug — the only
"real" fix would be hardcoding a target layout assumption, which is the
wrong instinct for a general-purpose HID tool.

### ST7735 display

Uses Bodmer/TFT_eSPI, configured via build flags (not a `User_Setup.h`
file) copied verbatim from the library's own bundled
`User_Setups/Setup209_LilyGo_T_Dongle_S3.h` — this exact hardware already
has an official, tested config upstream, so there was no need to derive
pin/panel-variant settings from scratch. The 887x1774 source logo
(`/mnt/Storage/Coding/Misc/Mascot/logo-80-160.png`, aspect ratio already
matching the 80x160 panel exactly) was converted to a `PROGMEM` RGB565 C
array (`src/logo.h`) via:
```
convert logo-80-160.png -resize 80x160! -depth 8 RGB:logo.raw
```
followed by a small Python script packing each 3-byte RGB pixel into a
16-bit `0bRRRRRGGGGGGBBBBB` value (see git history for the exact script).

Two real bugs were found and fixed empirically, in order:

1. **Hang on `tft.init()`'s first SPI transaction.** Confirmed via a
   host-independent diagnostic: since the hang also somehow prevented USB
   CDC from coming up (no serial log reachable during the hang), the RGB
   LED was pressed into service as a poor-man's log instead — distinct
   colors set immediately before each risky call, so whichever color the
   LED froze on identified exactly which call hung (froze on the color set
   right before `tft.init()`). Root-caused by reading TFT_eSPI's own
   ESP32-S3 driver source (`Processors/TFT_eSPI_ESP32_S3.h/.c`, fetched
   locally by PlatformIO's `lib_deps`, not guessed from memory): the
   library's SPI-busy-check macro polls a *raw hardware register pointer*
   computed for a specific SPI host (`FSPI`/SPI2 by default), and something
   about that host's state on this board/core combination left the busy
   bit stuck. Fixed with the `-DUSE_HSPI_PORT` build flag, forcing the
   library onto the alternate host (`HSPI`/SPI3) instead — a documented,
   commonly-cited workaround for TFT_eSPI-on-ESP32-S3 hangs. This was
   tested empirically (add the flag, reflash, observe) rather than fully
   root-caused at the register level, since that would need live JTAG
   debugging this session didn't have set up.
2. **Wrong colors (cyan rendered as yellow).** `pushImage()` expects
   big-endian pixel words by default; the generated array is plain
   little-endian `uint16_t` (native ESP32 byte order). The fix is
   `tft.setSwapBytes(true)` before `pushImage()` — NOT touching the pixel
   data. (A wrong first attempt pre-swapped the R/B channels in the source
   data instead, which combined with the *real* underlying byte-order bug
   to produce a different wrong color, purple — a useful confirmation that
   the bug was byte-order, not channel-order, once the math was checked.)

### The upload workflow's real quirk: no auto-reset circuit

Every single reflash in this session needed the same manual two-step
dance, and this is a permanent fact about this hardware, not a one-off
glitch: the T-Dongle S3 is a bare USB-A-plug dongle with no auto-reset
transistor pair (the RTS/DTR-to-EN/BOOT circuit normal dev boards have).
`esptool`'s software auto-reset-into-bootloader trick is unreliable here
(fails with "No serial data received" more often than not once the app
has been running for a while — some subsequent USB CDC session against
the running app is enough to leave the state where it stops working). The
reliable procedure every time:

1. **To flash**: hold the button, unplug+replug the dongle while holding
   it, keep holding ~2 more seconds, release. This forces GPIO0 low during
   the chip's own power-on reset, entering the ROM bootloader
   deterministically (`esptool`'s own connect handshake needs this to
   succeed at all).
2. **After a successful upload**: `esptool`'s post-upload "hard reset via
   RTS pin" step frequently doesn't actually leave bootloader mode either
   (confirmed via `lsusb -d 303a:` showing PID `0x1001` "Espressif USB
   JTAG/serial debug unit" — the ROM's own identity, not the app's). A
   second, *plain* unplug/replug (no button this time) is needed to force
   a clean power-on boot into the newly-flashed app.

## Next steps (not started)

Wiring actual keystroke-injection capability behind Reconclave's
signed-scope trust model once this device has a network stack —
advertised, logged, never autonomous. Also worth adding: a repeating
serial heartbeat already exists in `loop()` for future live-serial
debugging sessions.
