// Standalone verification tool for the Wi-Fi scanner - prints observed
// access points so the parser can be checked against real `iwlist` output
// before it's wired into the app. Not part of the Reconclave app itself.
#include <cstdio>

#include "src/wifi_scanner.h"

int main(int argc, char** argv) {
  const char* interface = argc > 1 ? argv[1] : "wlan0";

  std::vector<reconclave::WifiObservation> observations;
  std::string error;
  if (!reconclave::scanWifi(interface, observations, error)) {
    std::fprintf(stderr, "scan failed: %s\n", error.c_str());
    return 1;
  }

  std::printf("ssid,bssid,channel,rssi_dbm,security\n");
  for (const auto& ap : observations) {
    std::printf("%s,%s,%d,%d,%s\n", ap.ssid.c_str(), ap.bssid.c_str(), ap.channel, ap.rssi_dbm,
                ap.secured ? "secured" : "open");
  }
  std::printf("(%zu networks)\n", observations.size());
  return 0;
}
