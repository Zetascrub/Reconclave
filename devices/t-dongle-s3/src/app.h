// App core for the fleet node: owns services, holds modules, runs the
// begin/loop lifecycle. Bring-up order: base services -> node identity ->
// network (needs the id for mDNS) -> HTTP server -> module registration.
#pragma once

#include <vector>

#include "framework.h"
#include "node_service.h"
#include "radio_manager.h"
#include "status_led.h"
#include "usb_manager.h"

namespace reconclave {

class App {
 public:
  App();
  void addModule(Module& module);
  void begin();
  void loop();

 private:
  // Reads newline-terminated serial commands. Supported:
  //   wifi <ssid> <pass>   store credentials in NVS and reboot to join
  //   status               print id / link / ip
  void pollSerialCommands();
  String serial_line_;

  UsbManager usb_;
  RadioManager radio_;
  NodeService node_;
  StatusLed led_;
  InputService input_;
  Config config_;
  TriggerPolicy trigger_;
  Services services_;
  std::vector<Module*> modules_;
};

}  // namespace reconclave
