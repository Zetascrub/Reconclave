// Userspace driver for the TCA8418 keypad-scan controller on LILYGO's
// T-Display K230 keyboard kit.
//
// The stock image has no kernel input driver for this chip
// (CONFIG_KEYBOARD_TCA8418 is not set, confirmed on-device, and
// /lib/modules is empty - no loadable modules at all), so this talks to it
// directly over Linux's standard i2c-dev interface rather than requiring a
// kernel/device-tree change. Register map, init sequence, and key-code
// decoding are ported from the mainline Linux driver
// (drivers/input/keyboard/tca8418_keypad.c) against the same chip family,
// adapted to poll REG_KEY_LCK_EC instead of using the chip's interrupt
// line, since that needs no GPIO/IRQ wiring beyond the I2C bus.
//
// Confirmed on this specific board: chip present on /dev/i2c-0 at address
// 0x37 (found via i2cdetect; the vendor demo app at
// /root/app/k230_phone_ui also references TCA8418 on /dev/i2c-0 as one of
// its two fixed-bus options). The kernel driver's example wiring uses 0x34;
// TCA8418's ADDR strap selects among a small set of addresses, so a
// different LILYGO board revision landing on 0x37 is unsurprising.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace reconclave {

struct KeyEvent {
  int row = 0;
  int col = 0;
  bool pressed = false;
};

using KeyEventHandler = std::function<void(const KeyEvent&)>;

class Tca8418Keyboard {
 public:
  ~Tca8418Keyboard();

  // Opens `i2c_device` (e.g. "/dev/i2c-0"), selects `address` (e.g. 0x37),
  // and configures the chip for the full 8x10 matrix this keyboard kit
  // uses. Returns false on failure (device missing, wrong address, I2C
  // error).
  bool start(const std::string& i2c_device, std::uint8_t address);

  // Drains every pending FIFO event and invokes `handler` for each. Safe to
  // call on a timer/poll interval; does nothing if the FIFO is empty.
  void poll(const KeyEventHandler& handler);

  // Hardware-verification-only: read/write an arbitrary register, exposed
  // for kbtest.cpp's raw-bytes diagnostic mode. Not used by poll() callers.
  bool debugReadReg(std::uint8_t reg, std::uint8_t& value) { return readReg(reg, value); }
  bool debugWriteReg(std::uint8_t reg, std::uint8_t value) { return writeReg(reg, value); }

 private:
  int fd_ = -1;

  bool writeReg(std::uint8_t reg, std::uint8_t value);
  bool readReg(std::uint8_t reg, std::uint8_t& value);
};

}  // namespace reconclave
