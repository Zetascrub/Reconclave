// Passive Wi-Fi discovery, matching devices/cardputer-adv's
// `radio.wifi.scan` capability and CSV evidence shape
// (ssid,bssid,channel,rssi_dbm,security,observer_node - see
// saveWifiEvidence() in devices/cardputer-adv/src/main.cpp).
//
// Verified working on real K230 hardware via `iwlist wlan0 scanning`
// (confirmed real access points returned with SSID/channel/frequency/RSSI).
// Runs that tool directly via fork+exec (no shell), matching the project's
// preference for shell-free argument construction elsewhere
// (tools/desktop-node/tool_runner.py).
//
// Like Cardputer's `radio.wifi.scan`, this capability is advertised but
// deliberately not wired to remote dispatch yet: docs/capabilities.md
// classifies it as `Assessment` permission (not `system.info`'s
// read-only/non-sensitive class), so it needs engagement-scope enforcement
// before it should be remotely triggerable - see devices/k230/README.md.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace reconclave {

struct WifiObservation {
  std::string ssid;
  std::string bssid;
  int channel = 0;
  int frequency_mhz = 0;
  int rssi_dbm = 0;
  int quality_percent = 0;
  bool secured = false;
  std::string security{"open"};
};

struct WifiSurveySummary {
  int total = 0;
  int open = 0;
  int hidden = 0;
  int two_ghz = 0;
  int five_ghz = 0;
  int strongest_rssi_dbm = -100;
  int best_24_channel = 1;
  int channel_load_1 = 0;
  int channel_load_6 = 0;
  int channel_load_11 = 0;
};

// Runs a passive scan on `interface` (e.g. "wlan0"). On success returns
// true with `out` populated (possibly empty, if no APs were seen); on
// failure returns false with a description in `error`.
bool scanWifi(const std::string& interface, std::vector<WifiObservation>& out, std::string& error);

WifiSurveySummary analyseWifi(const std::vector<WifiObservation>& observations);

}  // namespace reconclave
