// Userspace driver for the TCA8418 keypad-scan controller on LILYGO's
// T-Display K230 keyboard kit.
//
// This chip is NOT on a kernel-visible I2C bus - confirmed two ways: (1)
// empirically, driving it as if it were at /dev/i2c-0 address 0x37 (an
// earlier version of this file's belief) read back a flat, unresponsive
// register (count=0x00 for the whole duration of a real key-press test -
// see kbtest.cpp's --raw mode and devices/k230/README.md); (2) that address
// is independently confirmed by /sys/bus/i2c/devices/0-0037/name to be the
// GC2093 camera sensor, not the keyboard. The real bus is a
// software-bit-banged pair of ordinary GPIO lines - confirmed by running
// the vendor's own k230_phone_ui live on this device (see README) and
// reading its own startup log: "[keyboard-base] Detected ... on I2C4
// SDA47/SCL46" and "[extension-keyboard] enabled: TCA8418 IRQ active at
// 0x34 GPIO42". Global GPIO 46/47 map to gpiochip1 line 14/15 (two 32-line
// chips, gpiochip0 = GPIO0-31, confirmed via `gpioinfo`).
//
// The bit-bang I2C master (BitbangI2c below) switches IO46/47 from their
// boot-time I2C4 alternate function to GPIO mode and requests both GPIOs
// as open-drain outputs, matching LILYGO's implementation. Writing logical
// high therefore releases the line to its pull-up, allowing slaves to
// drive ACK and data bits without electrical contention.
//
// Register map, init sequence, and key-code decoding are ported from the
// mainline Linux driver (drivers/input/keyboard/tca8418_keypad.c) against
// the same chip family, adapted to poll REG_KEY_LCK_EC instead of using the
// chip's interrupt line, since that needs no separate GPIO/IRQ wiring
// beyond the two bus lines already in use.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

struct gpiod_chip;
struct gpiod_line_request;

namespace reconclave {

// Bit-banged I2C master over two libgpiod v2 lines. Not TCA8418-specific -
// a small, general single-master, no-clock-stretching-support transport.
class BitbangI2c {
 public:
  ~BitbangI2c();

  // Opens `chip_path` (e.g. "/dev/gpiochip1") and requests `scl_offset`/
  // `sda_offset` as a pair of GPIO lines making up the bus, both initially
  // idle-high. Returns false on failure (chip/lines unavailable).
  bool open(const char* chip_path, unsigned scl_offset, unsigned sda_offset);

  // Writes `data` to `address` (a full I2C write transaction: START,
  // address+W, data bytes each with an ACK check, STOP). Returns false if
  // any byte goes unacknowledged or the bus otherwise misbehaves.
  bool writeBytes(std::uint8_t address, const std::uint8_t* data, std::size_t len);

  // Writes `write_data` (typically a single register address byte), then a
  // repeated START and reads `read_len` bytes back from `address` into
  // `read_data`, NACKing (and so ending the read after) the last byte per
  // the standard I2C convention. Returns false on any unacknowledged byte.
  bool writeThenRead(std::uint8_t address, const std::uint8_t* write_data, std::size_t write_len,
                     std::uint8_t* read_data, std::size_t read_len);

 private:
  gpiod_chip* chip_ = nullptr;
  gpiod_line_request* request_ = nullptr;
  unsigned scl_offset_ = 0;
  unsigned sda_offset_ = 0;
  bool owns_i2c4_iomux_ = false;

  void sclHigh();
  void sclLow();
  void sdaDriveLow();   // Actively drives the open-drain SDA line low.
  void sdaRelease();    // Releases SDA; the bus pull-up supplies logic high.
  bool sdaRead();       // Reads SDA - only meaningful right after sdaRelease().

  void startCondition();
  void stopCondition();
  bool writeBit(bool bit);
  bool readBit();
  bool writeByte(std::uint8_t byte);   // Returns the ACK bit (true = acked).
  std::uint8_t readByte(bool ack);     // `ack`: whether *we* ack this byte.
};

struct KeyEvent {
  int row = 0;
  int col = 0;
  bool pressed = false;
};

using KeyEventHandler = std::function<void(const KeyEvent&)>;

class Tca8418Keyboard {
 public:
  // Opens the bit-banged bus (gpiochip `chip_path`, `scl_offset`/
  // `sda_offset` - defaults match this board's confirmed wiring, see the
  // file header) and selects `address` (0x34, confirmed via
  // k230_phone_ui's own log). Returns false on failure (chip/lines
  // unavailable, or the chip doesn't answer at `address`).
  bool start(std::uint8_t address = 0x34, const char* chip_path = "/dev/gpiochip1",
            unsigned scl_offset = 14, unsigned sda_offset = 15);

  // Drains every pending FIFO event and invokes `handler` for each. Safe to
  // call on a timer/poll interval; does nothing if the FIFO is empty.
  void poll(const KeyEventHandler& handler);

  // Read a little-endian 16-bit register from another device sharing the
  // keyboard-base I2C4 bus. The UI owns this bus through this object, so
  // the fuel gauge can be sampled without a second GPIO claimant racing
  // the keyboard driver.
  bool readWord(std::uint8_t address, std::uint8_t reg, std::uint16_t& value);
  bool readByte(std::uint8_t address, std::uint8_t reg, std::uint8_t& value);

  // Hardware-verification-only: read/write an arbitrary register, exposed
  // for kbtest.cpp's raw-bytes diagnostic mode. Not used by poll() callers.
  bool debugReadReg(std::uint8_t reg, std::uint8_t& value) { return readReg(reg, value); }
  bool debugWriteReg(std::uint8_t reg, std::uint8_t value) { return writeReg(reg, value); }

 private:
  BitbangI2c bus_;
  std::uint8_t address_ = 0x34;

  bool writeReg(std::uint8_t reg, std::uint8_t value);
  bool readReg(std::uint8_t reg, std::uint8_t& value);
};

}  // namespace reconclave
