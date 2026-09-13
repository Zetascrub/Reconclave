#include "tca8418_keyboard.h"

#include <gpiod.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace reconclave {

namespace {

// A few microseconds of settle time per half-clock - generous relative to
// the TCA8418's actual timing requirements (standard-mode I2C, a few
// hundred nanoseconds), but syscall overhead on the real GPIO toggles
// already dominates this, so there's no throughput cost to being
// conservative here.
constexpr int kHalfClockDelayUs = 5;
void settle() { usleep(kHalfClockDelayUs); }

// IO46/47 boot in their I2C4 alternate function. libgpiod can reserve the
// corresponding GPIO lines without changing that K230 IOMUX selection, in
// which case every apparent GPIO transaction is disconnected from the pins.
// This is the same register sequence used by LILYGO's k230_phone_ui.
constexpr off_t kIomuxBase = 0x91105000;
constexpr std::size_t kIomuxSize = 0x1000;
constexpr unsigned kIo46Offset = 46U * 4U;
constexpr unsigned kIo47Offset = 47U * 4U;

std::uint32_t i2c4IomuxValue(unsigned selection) {
  return (selection << 11U) | (1U << 8U) | (1U << 7U) | (8U << 1U) | 1U;
}

bool setI2c4Iomux(unsigned selection) {
  const int fd = ::open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
  if (fd < 0) return false;
  void* map = mmap(nullptr, kIomuxSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, kIomuxBase);
  ::close(fd);
  if (map == MAP_FAILED) return false;
  auto* regs = static_cast<volatile std::uint32_t*>(map);
  regs[kIo46Offset / 4U] = i2c4IomuxValue(selection);
  regs[kIo47Offset / 4U] = i2c4IomuxValue(selection);
  munmap(map, kIomuxSize);
  return true;
}

// LILYGO's keyboard-base reference implementation resets the controller
// before configuring its matrix: GPIO43 low for 3 ms, then high for 12 ms.
// Global GPIO43 is gpiochip1 line 11 on this two-bank K230 layout.
bool resetController() {
  gpiod_chip* chip = gpiod_chip_open("/dev/gpiochip1");
  if (chip == nullptr) return false;
  gpiod_line_settings* settings = gpiod_line_settings_new();
  gpiod_line_config* lines = gpiod_line_config_new();
  gpiod_request_config* request_config = gpiod_request_config_new();
  constexpr unsigned int kResetOffset = 11;
  if (settings == nullptr || lines == nullptr || request_config == nullptr) {
    if (settings != nullptr) gpiod_line_settings_free(settings);
    if (lines != nullptr) gpiod_line_config_free(lines);
    if (request_config != nullptr) gpiod_request_config_free(request_config);
    gpiod_chip_close(chip);
    return false;
  }
  gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT);
  gpiod_line_settings_set_output_value(settings, GPIOD_LINE_VALUE_INACTIVE);
  gpiod_line_config_add_line_settings(lines, &kResetOffset, 1, settings);
  gpiod_request_config_set_consumer(request_config, "reconclave-tca8418-reset");
  gpiod_line_request* request = gpiod_chip_request_lines(chip, request_config, lines);
  gpiod_line_settings_free(settings);
  gpiod_line_config_free(lines);
  gpiod_request_config_free(request_config);
  if (request == nullptr) {
    gpiod_chip_close(chip);
    return false;
  }
  usleep(3000);
  const bool raised = gpiod_line_request_set_value(
      request, kResetOffset, GPIOD_LINE_VALUE_ACTIVE) == 0;
  usleep(12000);
  gpiod_line_request_release(request);
  gpiod_chip_close(chip);
  return raised;
}

}  // namespace

BitbangI2c::~BitbangI2c() {
  if (request_ != nullptr) gpiod_line_request_release(request_);
  if (chip_ != nullptr) gpiod_chip_close(chip_);
  if (owns_i2c4_iomux_) setI2c4Iomux(3);  // Restore the BSP's I2C4 function.
}

bool BitbangI2c::open(const char* chip_path, unsigned scl_offset, unsigned sda_offset) {
  // The keyboard-base bus is physically IO46/47. Put those pads in GPIO
  // mode before asking libgpiod for gpiochip1 offsets 14/15.
  owns_i2c4_iomux_ = std::strcmp(chip_path, "/dev/gpiochip1") == 0 &&
                      scl_offset == 14 && sda_offset == 15;
  if (owns_i2c4_iomux_ && !setI2c4Iomux(0)) {
    std::fprintf(stderr, "failed to select GPIO mode for IO46/47 I2C bus\n");
    owns_i2c4_iomux_ = false;
    return false;
  }
  chip_ = gpiod_chip_open(chip_path);
  if (chip_ == nullptr) {
    std::fprintf(stderr, "gpiod_chip_open(%s) failed: %s\n", chip_path, strerror(errno));
    return false;
  }

  gpiod_line_settings* settings = gpiod_line_settings_new();
  gpiod_line_config* line_config = gpiod_line_config_new();
  gpiod_request_config* request_config = gpiod_request_config_new();
  if (settings == nullptr || line_config == nullptr || request_config == nullptr) {
    if (settings != nullptr) gpiod_line_settings_free(settings);
    if (line_config != nullptr) gpiod_line_config_free(line_config);
    if (request_config != nullptr) gpiod_request_config_free(request_config);
    gpiod_chip_close(chip_);
    chip_ = nullptr;
    return false;
  }

  // Both lines start as outputs, idle-high - the bus's resting state.
  gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT);
  gpiod_line_settings_set_output_value(settings, GPIOD_LINE_VALUE_ACTIVE);
  gpiod_line_settings_set_drive(settings, GPIOD_LINE_DRIVE_OPEN_DRAIN);
  const unsigned int offsets[2] = {scl_offset, sda_offset};
  gpiod_line_config_add_line_settings(line_config, offsets, 2, settings);
  gpiod_request_config_set_consumer(request_config, "reconclave-tca8418");

  request_ = gpiod_chip_request_lines(chip_, request_config, line_config);
  gpiod_line_settings_free(settings);
  gpiod_line_config_free(line_config);
  gpiod_request_config_free(request_config);

  if (request_ == nullptr) {
    std::fprintf(stderr, "gpiod_chip_request_lines(%s, scl=%u, sda=%u) failed: %s\n", chip_path,
                 scl_offset, sda_offset, strerror(errno));
    gpiod_chip_close(chip_);
    chip_ = nullptr;
    return false;
  }
  scl_offset_ = scl_offset;
  sda_offset_ = sda_offset;
  return true;
}

void BitbangI2c::sclHigh() {
  gpiod_line_request_set_value(request_, scl_offset_, GPIOD_LINE_VALUE_ACTIVE);
  settle();
}

void BitbangI2c::sclLow() {
  gpiod_line_request_set_value(request_, scl_offset_, GPIOD_LINE_VALUE_INACTIVE);
  settle();
}

void BitbangI2c::sdaDriveLow() {
  // SDA is already configured as an output whenever this is called (either
  // from open()'s initial state, or restored by the write path below) -
  // just set the value.
  gpiod_line_request_set_value(request_, sda_offset_, GPIOD_LINE_VALUE_INACTIVE);
  settle();
}

void BitbangI2c::sdaRelease() {
  // The line request is open-drain: ACTIVE releases the pad and its pull-up
  // supplies logic high, while still allowing a slave to pull ACK/data low.
  gpiod_line_request_set_value(request_, sda_offset_, GPIOD_LINE_VALUE_ACTIVE);
  settle();
}

bool BitbangI2c::sdaRead() {
  return gpiod_line_request_get_value(request_, sda_offset_) == GPIOD_LINE_VALUE_ACTIVE;
}

void BitbangI2c::startCondition() {
  // Both idle-high already (or restored to that state by the previous
  // stopCondition()/open()) - SDA falling while SCL is high is a START.
  sclHigh();
  sdaDriveLow();
  sclLow();
}

void BitbangI2c::stopCondition() {
  sdaDriveLow();
  sclHigh();
  sdaRelease();  // SDA rising while SCL is high is a STOP.
}

bool BitbangI2c::writeBit(bool bit) {
  if (bit) {
    sdaRelease();
  } else {
    sdaDriveLow();
  }
  sclHigh();
  sclLow();
  return true;
}

bool BitbangI2c::readBit() {
  sclHigh();
  const bool value = sdaRead();
  sclLow();
  return value;
}

bool BitbangI2c::writeByte(std::uint8_t byte) {
  for (int bit = 7; bit >= 0; --bit) writeBit((byte >> bit) & 1);
  sdaRelease();
  const bool acked = !readBit();  // ACK = slave pulls SDA low.
  return acked;
}

std::uint8_t BitbangI2c::readByte(bool ack) {
  sdaRelease();
  std::uint8_t value = 0;
  for (int bit = 7; bit >= 0; --bit) {
    sclHigh();
    if (sdaRead()) value |= (1 << bit);
    sclLow();
  }
  if (ack) {
    sdaDriveLow();
    sclHigh();
    sclLow();
    sdaRelease();
  } else {
    sdaRelease();
    sclHigh();
    sclLow();
  }
  return value;
}

bool BitbangI2c::writeBytes(std::uint8_t address, const std::uint8_t* data, std::size_t len) {
  if (request_ == nullptr) return false;
  // Other K230 subsystems can re-apply the DT's I2C4 pinctrl state during
  // application startup. LILYGO's driver therefore selects GPIO mode for
  // each software-I2C transaction rather than assuming the setting remains
  // unchanged after open(). Do likewise so the long-lived UI is reliable.
  if (owns_i2c4_iomux_ && !setI2c4Iomux(0)) return false;
  startCondition();
  bool ok = writeByte(static_cast<std::uint8_t>(address << 1));  // W bit = 0.
  for (std::size_t i = 0; ok && i < len; ++i) ok = writeByte(data[i]);
  stopCondition();
  return ok;
}

bool BitbangI2c::writeThenRead(std::uint8_t address, const std::uint8_t* write_data,
                               std::size_t write_len, std::uint8_t* read_data,
                               std::size_t read_len) {
  if (request_ == nullptr) return false;
  if (owns_i2c4_iomux_ && !setI2c4Iomux(0)) return false;
  startCondition();
  bool ok = writeByte(static_cast<std::uint8_t>(address << 1));  // W bit = 0.
  for (std::size_t i = 0; ok && i < write_len; ++i) ok = writeByte(write_data[i]);
  if (ok) {
    startCondition();  // Repeated START.
    ok = writeByte(static_cast<std::uint8_t>((address << 1) | 1));  // R bit = 1.
  }
  for (std::size_t i = 0; ok && i < read_len; ++i) {
    read_data[i] = readByte(/*ack=*/i + 1 < read_len);
  }
  stopCondition();
  return ok;
}

namespace {

// Register addresses (drivers/input/keyboard/tca8418_keypad.c).
constexpr std::uint8_t kRegCfg = 0x01;
constexpr std::uint8_t kRegIntStat = 0x02;
constexpr std::uint8_t kRegKeyLckEc = 0x03;
constexpr std::uint8_t kRegKeyEventA = 0x04;
constexpr std::uint8_t kRegKpGpio1 = 0x1D;
constexpr std::uint8_t kRegKpGpio2 = 0x1E;
constexpr std::uint8_t kRegKpGpio3 = 0x1F;
constexpr std::uint8_t kRegDebounceDis1 = 0x29;
constexpr std::uint8_t kRegDebounceDis2 = 0x2A;
constexpr std::uint8_t kRegDebounceDis3 = 0x2B;

// CFG bits.
constexpr std::uint8_t kCfgOvrFlowM = 1 << 5;
constexpr std::uint8_t kCfgIntCfg = 1 << 4;
constexpr std::uint8_t kCfgOvrFlowIen = 1 << 3;
constexpr std::uint8_t kCfgKeIen = 1 << 0;

// KEY_LCK_EC[3:0] is the FIFO event count (0..10). Masking only three
// bits makes a completely valid count of eight look empty and leaves the
// queue permanently wedged once it reaches that depth.
constexpr std::uint8_t kKeyLckEcKec = 0x0F;     // FIFO entry count mask.
constexpr std::uint8_t kKeyEventCode = 0x7f;    // Key index, 1-80.
constexpr std::uint8_t kKeyEventValue = 0x80;   // 1 = pressed, 0 = released.
constexpr int kMaxCols = 10;

// Matrix scan-enable mask - see kbtest.cpp/README for how this was
// determined against real hardware once the bus itself was actually
// correct (this board's variant doesn't wire row 7).
constexpr std::uint8_t kMatrixGpio1 = 0x7F;  // Rows R0-R6; R7 excluded (floating).
constexpr std::uint8_t kMatrixGpio2 = 0xFF;
constexpr std::uint8_t kMatrixGpio3 = 0x03;

}  // namespace

bool Tca8418Keyboard::writeReg(std::uint8_t reg, std::uint8_t value) {
  const std::uint8_t data[2] = {reg, value};
  return bus_.writeBytes(address_, data, sizeof(data));
}

bool Tca8418Keyboard::readReg(std::uint8_t reg, std::uint8_t& value) {
  return bus_.writeThenRead(address_, &reg, 1, &value, 1);
}

bool Tca8418Keyboard::start(std::uint8_t address, const char* chip_path, unsigned scl_offset,
                            unsigned sda_offset) {
  address_ = address;
  if (!bus_.open(chip_path, scl_offset, sda_offset)) return false;
  if (!resetController()) {
    std::fprintf(stderr, "TCA8418 reset pulse on GPIO43 failed\n");
    return false;
  }

  // Confirm the chip actually answers before committing to init writes -
  // a wrong/absent address should fail start() cleanly rather than send
  // writes into the void.
  std::uint8_t probe = 0;
  if (!readReg(kRegKeyLckEc, probe)) return false;

  bool ok = true;
  ok &= writeReg(kRegKpGpio1, kMatrixGpio1);
  ok &= writeReg(kRegKpGpio2, kMatrixGpio2);
  ok &= writeReg(kRegKpGpio3, kMatrixGpio3);
  // Leave debounce enabled (0x00 = disabled-mask clear) - see the constant
  // comment above.
  ok &= writeReg(kRegDebounceDis1, 0x00);
  ok &= writeReg(kRegDebounceDis2, 0x00);
  ok &= writeReg(kRegDebounceDis3, 0x00);
  ok &= writeReg(kRegCfg, kCfgIntCfg | kCfgOvrFlowIen | kCfgOvrFlowM | kCfgKeIen);

  // Clear any stale interrupt-status bits and drain any FIFO backlog from
  // before this process started, so poll() starts from a clean slate.
  std::uint8_t int_stat = 0;
  if (readReg(kRegIntStat, int_stat)) writeReg(kRegIntStat, int_stat);
  std::uint8_t discard;
  for (int i = 0; i < 10; ++i) {
    std::uint8_t count = 0;
    if (!readReg(kRegKeyLckEc, count) || (count & kKeyLckEcKec) == 0) break;
    if (!readReg(kRegKeyEventA, discard)) break;
  }

  return ok;
}

void Tca8418Keyboard::poll(const KeyEventHandler& handler) {
  for (int guard = 0; guard < 80; ++guard) {
    std::uint8_t count = 0;
    if (!readReg(kRegKeyLckEc, count)) return;
    if ((count & kKeyLckEcKec) == 0) return;

    std::uint8_t reg = 0;
    if (!readReg(kRegKeyEventA, reg)) return;

    bool pressed = (reg & kKeyEventValue) != 0;
    std::uint8_t code = reg & kKeyEventCode;
    if (code == 0) return;  // No more valid events despite a nonzero count.

    // TCA8418 key numbering is 1-based (key 1 = row 0, col 0), so plain
    // code/10, code%10 needs the same off-by-one correction the kernel
    // driver applies before treating the result as 0-based row/col.
    int row = code / kMaxCols;
    int col = code % kMaxCols;
    if (col != 0) {
      col -= 1;
    } else {
      row -= 1;
      col = kMaxCols - 1;
    }

    handler(KeyEvent{row, col, pressed});
  }
}

bool Tca8418Keyboard::readWord(std::uint8_t address, std::uint8_t reg,
                               std::uint16_t& value) {
  for (int attempt = 0; attempt < 3; ++attempt) {
    std::uint8_t bytes[2]{};
    if (bus_.writeThenRead(address, &reg, 1, bytes, sizeof(bytes))) {
      value = static_cast<std::uint16_t>(bytes[0]) |
              (static_cast<std::uint16_t>(bytes[1]) << 8);
      return true;
    }
    usleep(1000);
  }
  return false;
}

bool Tca8418Keyboard::readByte(std::uint8_t address, std::uint8_t reg,
                               std::uint8_t& value) {
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (bus_.writeThenRead(address, &reg, 1, &value, 1)) return true;
    usleep(1000);
  }
  return false;
}

}  // namespace reconclave
