#include "radio_manager.h"

#include <ESPmDNS.h>
#include <WiFi.h>

namespace reconclave {
namespace {
// Management-AP passphrase (documented default). This gates the local
// setup/status UI; change and reflash where a fixed default isn't acceptable.
constexpr const char* kApPassword = "reconclave-setup";
}  // namespace

void RadioManager::init() {
  WiFi.mode(WIFI_AP_STA);  // netif up unconditionally; AP + STA together
  String mac = WiFi.softAPmacAddress();
  mac.replace(":", "");
  ap_ssid_ = "Zeta-Node-" + mac.substring(mac.length() - 4);
  const bool ap_up = WiFi.softAP(ap_ssid_.c_str(), kApPassword);
  Serial.printf("radio: management AP \"%s\" %s at %s\n", ap_ssid_.c_str(),
                ap_up ? "up" : "FAILED", WiFi.softAPIP().toString().c_str());
}

bool RadioManager::connect(const String& ssid, const String& pass, const String& device_id,
                           uint16_t port) {
  if (ssid.length() == 0) {
    Serial.println("radio: no wifi_ssid set - STA idle (configure via the web UI or serial)");
    return false;
  }
  WiFi.begin(ssid.c_str(), pass.c_str());
  const unsigned long deadline = millis() + 15000;
  while (WiFi.status() != WL_CONNECTED && millis() < deadline) delay(200);
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("radio: failed to join \"%s\"\n", ssid.c_str());
    return false;
  }
  Serial.printf("radio: joined \"%s\" as %s\n", ssid.c_str(), WiFi.localIP().toString().c_str());
  if (MDNS.begin(device_id.c_str())) {
    MDNS.addService("reconclave", "tcp", port);
    mdns_up_ = true;
    Serial.printf("radio: mDNS _reconclave._tcp on %u as %s\n", port, device_id.c_str());
  } else {
    Serial.println("radio: mDNS start failed");
  }
  return true;
}

bool RadioManager::staConnected() const { return WiFi.status() == WL_CONNECTED; }
IPAddress RadioManager::staIp() const { return WiFi.localIP(); }
IPAddress RadioManager::apIp() const { return WiFi.softAPIP(); }

}  // namespace reconclave
