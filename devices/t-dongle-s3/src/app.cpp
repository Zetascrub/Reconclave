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

void App::pollSerialCommands() {
  while (Serial.available() > 0) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\r') continue;
    if (c != '\n') {
      if (serial_line_.length() < 160) serial_line_ += c;
      continue;
    }
    String line = serial_line_;
    serial_line_ = "";
    line.trim();
    if (line.startsWith("wifi ")) {
      // "wifi <ssid> <pass>" — split on the first space after the command; the
      // password may contain spaces, the SSID may not.
      const String rest = line.substring(5);
      const int sep = rest.indexOf(' ');
      if (sep <= 0) {
        Serial.println("usage: wifi <ssid> <pass>");
        continue;
      }
      const String ssid = rest.substring(0, sep);
      const String pass = rest.substring(sep + 1);
      config_.setString("wifi_ssid", ssid);
      config_.setString("wifi_pass", pass);
      Serial.printf("wifi: stored SSID \"%s\" (%d-char pass) - rebooting to join\n",
                    ssid.c_str(), pass.length());
      delay(200);
      ESP.restart();
    } else if (line == "status") {
      Serial.printf("status: id=%s link=%s ip=%s\n", node_.deviceId().c_str(),
                    radio_.connected() ? "up" : "down", radio_.ip().toString().c_str());
    } else if (line.length() > 0) {
      Serial.println("commands: wifi <ssid> <pass> | status");
    }
  }
}

void App::loop() {
  const unsigned long now = millis();
  pollSerialCommands();
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
