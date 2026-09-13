#include "app.h"

#include <Arduino.h>

namespace reconclave {
namespace {
constexpr unsigned long kHeartbeatIntervalMs = 5000;
}  // namespace

App::App() : services_{usb_, radio_, node_, led_, input_, config_, trigger_} {}

void App::addModule(Module& module) { modules_.push_back(&module); }

void App::begin() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("reconclave t-dongle node: framework boot");

  config_.begin();
  led_.begin();
  input_.begin(pins::kButton);
  usb_.start();

  node_.beginIdentity(config_);

  const String ssid = config_.getString("wifi_ssid", "");
  const String pass = config_.getString("wifi_pass", "");
  radio_.begin(ssid, pass, node_.deviceId(), 80);

  node_.startServer(led_);

  for (Module* m : modules_) {
    Serial.printf("module: begin %s\n", m->capabilityId());
    m->begin(services_);
  }
  Serial.println("reconclave t-dongle node: ready");
}

void App::loop() {
  const unsigned long now = millis();
  input_.poll();
  node_.handleClient();
  for (Module* m : modules_) m->loop(services_);

  led_.setLinked(radio_.connected());
  led_.tick(now);

  static unsigned long next_heartbeat = 0;
  if (now >= next_heartbeat) {
    Serial.printf("heartbeat: id=%s link=%s ip=%s\n", node_.deviceId().c_str(),
                  radio_.connected() ? "up" : "down", radio_.ip().toString().c_str());
    next_heartbeat = now + kHeartbeatIntervalMs;
  }
}

}  // namespace reconclave
