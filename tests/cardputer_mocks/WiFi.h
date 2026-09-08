#pragma once
#include "IPAddress.h"
constexpr int WL_CONNECTED = 3;
struct TestWifi {
  int state = WL_CONNECTED;
  IPAddress address{192,168,1,2}, mask{255,255,255,0};
  int status() const { return state; }
  IPAddress localIP() const { return address; }
  IPAddress subnetMask() const { return mask; }
};
extern TestWifi WiFi;
