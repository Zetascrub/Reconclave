// T-Dongle-S3 milestones 2+3: USB HID keyboard enumeration (physical-button
// triggered only) plus the ST7735 display showing a boot logo.
//
// HID: this proves the device enumerates as a real USB keyboard and can
// type, using the *smallest* possible trigger: the device's own physical
// button. There is no network stack wired up yet, so there is no remote/
// autonomous path to a keystroke at all right now - firing requires a
// human standing at the device pressing its button. That's a deliberately
// conservative starting point, not the final design: once this device
// gains network connectivity, actual remote-triggered HID injection must
// be gated by the same signed-scope/trust model devices/k230's
// trust_policy.cpp already implements (verified scope + evidence logging),
// never a bare "message arrives, keys get typed" path. See
// devices/t-dongle-s3/README.md.
//
// The test payload below is deliberately inert: no Enter/Return key, no
// modifier keys, plain lowercase text - it cannot execute a focused shell
// command or trigger a keyboard shortcut, only visibly type a marker
// string wherever focus happens to be.
#include <Arduino.h>
#include <FastLED.h>
#include <TFT_eSPI.h>
#include <USB.h>
#include <USBHIDKeyboard.h>

#include "logo.h"

namespace {

// ST7735 80x160, backlight on GPIO38 - active-LOW (0=on, 1=off), confirmed
// from LILYGO's own examples/TFT_eSPI/TFT_eSPI.ino and examples/lcd/lcd.ino
// (both drive it manually rather than through TFT_eSPI's own TFT_BL macro,
// so this does the same). All other display pins/timing come from Bodmer/
// TFT_eSPI's own bundled Setup209_LilyGo_T_Dongle_S3.h, applied via build
// flags in platformio.ini.
constexpr int kBacklightPin = 38;
TFT_eSPI g_tft;

// APA102 (data+clock, NOT WS2812's single-wire protocol) - confirmed from
// Xinyuan-LilyGO/T-Dongle-S3's own examples/led/led.ino, not assumed from
// the single-pin "RGB LED" mentions some third-party pinout pages give.
constexpr int kLedDataPin = 40;
constexpr int kLedClockPin = 39;
CRGB g_led[1];

// See milestone 1's main.cpp history for why this is hardcoded rather than
// a `BOOT_PIN` macro: that macro comes from a LILYGO board-variant header
// PlatformIO doesn't have for this unlisted board.
constexpr int kButtonPin = 0;
constexpr unsigned long kBlinkIntervalMs = 500;
constexpr unsigned long kSendFlashMs = 200;
constexpr unsigned long kHeartbeatIntervalMs = 5000;

USBHIDKeyboard g_keyboard;
int g_send_count = 0;

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);  // Let the USB CDC side actually enumerate before printing.
  Serial.println();
  Serial.println("reconclave t-dongle-s3: milestones 2+3 (HID keyboard, display)");

  pinMode(kButtonPin, INPUT_PULLUP);

  // LED first, before anything display-related: an unambiguous, host-
  // independent "firmware got this far" signal in case TFT init hangs.
  FastLED.addLeds<APA102, kLedDataPin, kLedClockPin, BGR>(g_led, 1);
  FastLED.setBrightness(60);
  g_led[0] = CRGB::Blue;
  FastLED.show();

  pinMode(kBacklightPin, OUTPUT);
  digitalWrite(kBacklightPin, LOW);  // Active-low: LOW = backlight on.
  g_tft.init();
  g_tft.setRotation(0);  // Portrait, matching the logo's native 80x160.
  // pushImage() expects big-endian pixel words by default; our generated
  // array is plain little-endian uint16_t (native ESP32 byte order), which
  // without this came out as a scrambled color (confirmed empirically:
  // cyan rendered as yellow, and a wrong attempt to fix it by pre-swapping
  // R/B channels in the source data instead produced purple - the real
  // fix is telling the library the byte order, not touching pixel data).
  g_tft.setSwapBytes(true);
  g_tft.pushImage(0, 0, kLogoWidth, kLogoHeight, kLogoImage);

  // Device identity ("Reconclave"/"Reconclave T-Dongle-S3" - not spoofed as
  // any particular commercial keyboard) is set via USB_MANUFACTURER/
  // USB_PRODUCT build flags in platformio.ini, not here: ARDUINO_USB_CDC_
  // ON_BOOT already calls USB.begin() before setup() runs, so by now
  // USB.productName()/manufacturerName() would silently no-op.
  g_keyboard.begin();
}

void loop() {
  static unsigned long next_blink = 0;
  static unsigned long next_heartbeat = 0;
  static unsigned long flash_until = 0;
  static bool led_on = true;
  static bool last_button_state = true;  // INPUT_PULLUP: idle high.

  const unsigned long now = millis();
  const bool button_pressed = digitalRead(kButtonPin) == LOW;

  if (button_pressed != !last_button_state) {
    last_button_state = !button_pressed;
    if (button_pressed) {
      Serial.println("button: down");
    } else {
      // Fire only on release, i.e. a completed press-and-release, not on
      // every tick the button happens to be held.
      Serial.println("button: up - sending HID test string");
      g_send_count++;
      g_keyboard.print("reconclave-t-dongle-s3 hid-test-ok #");
      g_keyboard.print(g_send_count);
      flash_until = now + kSendFlashMs;
    }
  }

  if (now >= next_heartbeat) {
    Serial.printf("heartbeat: alive, sends=%d\n", g_send_count);
    next_heartbeat = now + kHeartbeatIntervalMs;
  }

  if (button_pressed) {
    g_led[0] = CRGB::Red;
    FastLED.show();
  } else if (now < flash_until) {
    g_led[0] = CRGB::Green;
    FastLED.show();
  } else if (now >= next_blink) {
    led_on = !led_on;
    g_led[0] = led_on ? CRGB::Blue : CRGB::Black;
    FastLED.show();
    next_blink = now + kBlinkIntervalMs;
  }
}
