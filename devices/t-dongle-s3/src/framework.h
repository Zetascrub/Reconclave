// Reconclave T-Dongle node firmware framework — core contracts.
//
// Same capability-module shape as the standalone ZetaDongle firmware (App core
// + shared services + self-registering modules), but this is a *fleet node*:
// actions are invoked by the coordinator over the authenticated protocol, so
// TriggerPolicy models an authenticated, scope-verified network trigger rather
// than only a physical button.
//
// Invariant: an action capability fires only if TriggerPolicy authorises it. A
// network trigger requires BOTH request authentication (HMAC) AND a verified
// signed engagement scope. Until signed-scope verification is implemented
// (milestone 2) scope_verified is always false, so HID is announced but
// fail-closed — it can never fire unscoped.
#pragma once

#include <Arduino.h>

namespace reconclave {

namespace pins {
constexpr int kButton = 0;     // GPIO0 boot-strap button (optional local trigger)
constexpr int kLedData = 40;   // APA102 data
constexpr int kLedClock = 39;  // APA102 clock
}  // namespace pins

enum class TriggerSource { PhysicalButton, Network, Timer, Unknown };

// Context for an authorisation decision. For a network trigger, both flags must
// be true; the node sets `authenticated` after the HMAC check and (in a future
// milestone) `scope_verified` after checking the signed scope-delegation token.
struct TriggerContext {
  TriggerSource source{TriggerSource::Unknown};
  bool authenticated{false};
  bool scope_verified{false};
};

class TriggerPolicy {
 public:
  bool authorize(const char* capability_id, const TriggerContext& ctx, String& reason) const {
    (void)capability_id;
    if (ctx.source == TriggerSource::PhysicalButton) return true;
    if (ctx.source == TriggerSource::Network) {
      if (!ctx.authenticated) {
        reason = "request not authenticated";
        return false;
      }
      if (!ctx.scope_verified) {
        reason = "signed engagement scope required";
        return false;
      }
      return true;
    }
    reason = "unauthorised trigger source";
    return false;
  }
};

class InputService {
 public:
  void begin(int button_pin) {
    pin_ = button_pin;
    pinMode(pin_, INPUT_PULLUP);
  }
  void poll() {
    pressed_now_ = digitalRead(pin_) == LOW;
    if (pressed_now_ != last_pressed_) {
      if (!pressed_now_) released_edge_ = true;
      last_pressed_ = pressed_now_;
    }
  }
  bool pressedNow() const { return pressed_now_; }
  bool consumePrimaryPress() {
    const bool e = released_edge_;
    released_edge_ = false;
    return e;
  }

 private:
  int pin_ = 0;
  bool last_pressed_ = false;
  bool pressed_now_ = false;
  bool released_edge_ = false;
};

class Config {
 public:
  void begin();
  uint32_t getUInt(const char* key, uint32_t fallback = 0);
  void setUInt(const char* key, uint32_t value);
  String getString(const char* key, const String& fallback = String());
  void setString(const char* key, const String& value);
};

// Services, injected into every module. Forward-declared here.
class UsbManager;
class RadioManager;
class NodeService;
class StatusLed;

struct Services {
  UsbManager& usb;
  RadioManager& radio;
  NodeService& node;
  StatusLed& led;
  InputService& input;
  Config& config;
  TriggerPolicy& trigger;
};

class Module {
 public:
  virtual ~Module() = default;
  virtual const char* capabilityId() const = 0;
  virtual void begin(Services& services) { (void)services; }
  virtual void loop(Services& services) { (void)services; }
};

}  // namespace reconclave
