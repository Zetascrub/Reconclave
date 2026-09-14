#include "radio_manager.h"

#include <ESPmDNS.h>
#include <WiFi.h>

namespace reconclave {

void RadioManager::init() {
  // Initialise the STA netif / TCP-IP stack up front, unconditionally. Reading
  // the MAC and starting the HTTP server both need this; without it the boot
  // crashes when no credentials are set.
  WiFi.mode(WIFI_STA);
}

bool RadioManager::connect(const String& ssid, const String& pass, const String& device_id,
                           uint16_t port) {
  if (ssid.length() == 0) {
    Serial.println("radio: no wifi_ssid set - node offline (serial: wifi <ssid> <pass>)");
    return false;
  }
  WiFi.begin(ssid.c_str(), pass.c_str());
  const unsigned long deadline = millis() + 15000;
  while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
    delay(200);
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("radio: failed to join \"%s\"\n", ssid.c_str());
    return false;
  }
  Serial.printf("radio: joined \"%s\" as %s\n", ssid.c_str(), WiFi.localIP().toString().c_str());

  // Advertise _reconclave._tcp so the coordinator discovers this node.
  if (MDNS.begin(device_id.c_str())) {
    MDNS.addService("reconclave", "tcp", port);
    mdns_up_ = true;
    Serial.printf("radio: mDNS advertising _reconclave._tcp on %u as %s\n", port,
                  device_id.c_str());
  } else {
    Serial.println("radio: mDNS start failed");
  }
  return true;
}

bool RadioManager::connected() const { return WiFi.status() == WL_CONNECTED; }
IPAddress RadioManager::ip() const { return WiFi.localIP(); }

}  // namespace reconclave
