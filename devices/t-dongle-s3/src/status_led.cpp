#include "status_led.h"

#include <Arduino.h>

#include "framework.h"  // pins

namespace reconclave {
namespace {
constexpr unsigned long kBlinkIntervalMs = 700;
}  // namespace

void StatusLed::begin() {
  FastLED.addLeds<APA102, pins::kLedData, pins::kLedClock, BGR>(led_, 1);
  FastLED.setBrightness(50);
  led_[0] = CRGB::Blue;
  FastLED.show();
}

void StatusLed::flash(const CRGB& color, unsigned long ms) {
  flash_color_ = color;
  flash_until_ = millis() + ms;
}

void StatusLed::tick(unsigned long now) {
  if (now < flash_until_) {
    led_[0] = flash_color_;
    FastLED.show();
  } else if (now >= next_blink_) {
    on_ = !on_;
    const CRGB base = linked_ ? CRGB(0, 205, 215) : CRGB::Blue;  // cyan when linked
    led_[0] = on_ ? base : CRGB::Black;
    FastLED.show();
    next_blink_ = now + kBlinkIntervalMs;
  }
}

}  // namespace reconclave
