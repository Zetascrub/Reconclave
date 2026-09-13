// Minimal APA102 status LED for the node (no display on this firmware). Colours
// signal coarse state: blue idle blink, cyan when the coordinator link is up,
// amber on a rejected/denied request, green on an accepted action.
#pragma once

#include <FastLED.h>

namespace reconclave {

class StatusLed {
 public:
  void begin();
  void tick(unsigned long now);
  void flash(const CRGB& color, unsigned long ms);
  void setLinked(bool linked) { linked_ = linked; }

 private:
  CRGB led_[1];
  bool linked_ = false;
  bool on_ = true;
  unsigned long next_blink_ = 0;
  unsigned long flash_until_ = 0;
  CRGB flash_color_ = CRGB::Black;
};

}  // namespace reconclave
