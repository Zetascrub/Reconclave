// T-Dongle-S3 milestone 1: toolchain + basic I/O proof. Confirms the
// PlatformIO build/upload pipeline works against real hardware and that
// the three onboard peripherals this device actually has respond as
// LILYGO's own examples say they should - no USB HID, no LCD, no network
// yet. See devices/t-dongle-s3/README.md for the pin sourcing (LILYGO's
// own example sketches, not third-party summaries or guesses) and for
// what's still ahead.
#include <Arduino.h>
#include <FastLED.h>

namespace {

// APA102 (data+clock, NOT WS2812's single-wire protocol) - confirmed from
// Xinyuan-LilyGO/T-Dongle-S3's own examples/led/led.ino, not assumed from
// the single-pin "RGB LED" mentions some third-party pinout pages give.
constexpr int kLedDataPin = 40;
constexpr int kLedClockPin = 39;
CRGB g_led[1];

// LILYGO's own usb_hid_keyboard.ino example uses a `BOOT_PIN` macro
// instead of a hardcoded number, but that macro comes from a board-variant
// header PlatformIO doesn't have for this unlisted board (we're building
// against the generic esp32-s3-devkitc-1 variant instead, same as
// devices/cardputer-adv does for its own unlisted M5Stack hardware) - GPIO0
// is the universal ESP32 boot-strap pin every source (LILYGO's examples,
// espboards.dev, HomeDing) agrees this button sits on, so it's hardcoded
// here rather than left depending on a macro this build doesn't define.
constexpr int kButtonPin = 0;
constexpr unsigned long kBlinkIntervalMs = 500;

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);  // Let the USB CDC side actually enumerate before printing.
  Serial.println();
  Serial.println("reconclave t-dongle-s3: milestone 1 (toolchain + I/O proof)");

  pinMode(kButtonPin, INPUT_PULLUP);

  FastLED.addLeds<APA102, kLedDataPin, kLedClockPin, BGR>(g_led, 1);
  FastLED.setBrightness(60);
  g_led[0] = CRGB::Blue;
  FastLED.show();
}

void loop() {
  static unsigned long next_blink = 0;
  static bool led_on = true;
  static bool last_button_state = true;  // INPUT_PULLUP: idle high.

  const bool button_pressed = digitalRead(kButtonPin) == LOW;
  if (button_pressed != !last_button_state) {
    Serial.println(button_pressed ? "button: down" : "button: up");
    last_button_state = !button_pressed;
  }

  if (millis() >= next_blink) {
    led_on = !led_on;
    g_led[0] = button_pressed ? CRGB::Red : (led_on ? CRGB::Blue : CRGB::Black);
    FastLED.show();
    next_blink = millis() + kBlinkIntervalMs;
  }
}
