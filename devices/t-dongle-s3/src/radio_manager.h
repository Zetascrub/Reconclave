// Radios for the node. Runs AP + STA concurrently (WIFI_AP_STA):
//   - a management AP (Zeta-Node-<mac4>) is ALWAYS up, so the setup/status web
//     UI is reachable even before WiFi is configured (Pineapple-style);
//   - STA joins the fleet network when credentials are set, and only then is
//     _reconclave._tcp advertised over mDNS for the coordinator.
// Bringing the netif up unconditionally in init() also avoids the offline-boot
// crash the STA-only early-return once caused.
#pragma once

#include <Arduino.h>
#include <IPAddress.h>

namespace reconclave {

class RadioManager {
 public:
  // Bring up AP+STA and start the always-on management AP. Call before reading
  // the MAC or starting any server.
  void init();

  // Join the fleet network (STA) if credentials are set, and advertise mDNS on
  // success. Empty credentials => STA idle, AP still up.
  bool connect(const String& ssid, const String& pass, const String& device_id, uint16_t port);

  bool staConnected() const;
  IPAddress staIp() const;
  const String& apSsid() const { return ap_ssid_; }
  IPAddress apIp() const;

 private:
  String ap_ssid_;
  bool mdns_up_ = false;
};

}  // namespace reconclave
