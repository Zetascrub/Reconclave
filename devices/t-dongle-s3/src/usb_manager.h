// Owns the USB HID keyboard used by the HID capability. USB.begin() is already
// called by the core before setup() (ARDUINO_USB_CDC_ON_BOOT=1); start() adds
// the keyboard on top of the running CDC device.
#pragma once

#include <USBHIDKeyboard.h>

namespace reconclave {

class UsbManager {
 public:
  void start();
  USBHIDKeyboard& keyboard() { return keyboard_; }

 private:
  USBHIDKeyboard keyboard_;
};

}  // namespace reconclave
