#include "usb_manager.h"

#include <Arduino.h>

namespace reconclave {

void UsbManager::start() {
  keyboard_.begin();
  Serial.println("usb: HID keyboard up");
}

}  // namespace reconclave
