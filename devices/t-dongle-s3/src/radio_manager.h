// Joins the fleet network (STA) and advertises the node over mDNS as
// _reconclave._tcp, so the coordinator discovers it the same way it discovers
// the P4/Cardputer. Credentials come from NVS (Config keys wifi_ssid/wifi_pass);
// a setup flow to enter them is a future item — for now set them once over
// serial/NVS. (Unlike the standalone ZetaDongle, this is STA, not an AP.)
#pragma once

#include <Arduino.h>
#include <IPAddress.h>

namespace reconclave {

class RadioManager {
 public:
  // Connects to `ssid`/`pass` (bounded wait) and, on success, advertises
  // `_reconclave._tcp` on `port` with instance name `device_id`.
  bool begin(const String& ssid, const String& pass, const String& device_id, uint16_t port);

  bool connected() const;
  IPAddress ip() const;

 private:
  bool mdns_up_ = false;
};

}  // namespace reconclave
