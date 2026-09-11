#include "tca8418_keyboard.h"

#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace reconclave {

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

constexpr std::uint8_t kKeyLckEcKec = 0x7;      // FIFO entry count mask.
constexpr std::uint8_t kKeyEventCode = 0x7f;    // Key index, 1-80.
constexpr std::uint8_t kKeyEventValue = 0x80;   // 1 = pressed, 0 = released.
constexpr int kMaxCols = 10;

// LILYGO's own tca8418_keyboard_lvgl[] table (T-Display-K230_canmv_rt) has
// 80 entries, matching the chip's full 8x10 matrix, so that was the
// starting point here too: GPIO1 = rows R0-R7 (0xFF), GPIO2 = columns
// C0-C7 (0xFF), GPIO3 = remaining columns C8-C9 (0x03). On real hardware
// that produced a continuous flood of "row=7 col=0" events from the very
// first poll, before any real keypress and regardless of what was actually
// typed - the signature of an unconnected/floating input, not a real key.
// This board's keyboard variant evidently does not wire all 8 rows;
// row 7 is excluded from the scan mask (bit 7 clear) rather than treated
// as a floating keypad input.
constexpr std::uint8_t kMatrixGpio1 = 0x7F;  // Rows R0-R6; R7 excluded (floating).
constexpr std::uint8_t kMatrixGpio2 = 0xFF;
constexpr std::uint8_t kMatrixGpio3 = 0x03;

}  // namespace

Tca8418Keyboard::~Tca8418Keyboard() {
  if (fd_ >= 0) close(fd_);
}

bool Tca8418Keyboard::writeReg(std::uint8_t reg, std::uint8_t value) {
  std::uint8_t buf[2] = {reg, value};
  ssize_t n = write(fd_, buf, sizeof(buf));
  if (n != static_cast<ssize_t>(sizeof(buf))) {
    std::fprintf(stderr, "writeReg(0x%02x, 0x%02x) failed: n=%zd errno=%d (%s)\n", reg, value, n,
                 errno, strerror(errno));
    return false;
  }
  return true;
}

bool Tca8418Keyboard::readReg(std::uint8_t reg, std::uint8_t& value) {
  ssize_t wn = write(fd_, &reg, 1);
  if (wn != 1) {
    std::fprintf(stderr, "readReg(0x%02x) write failed: n=%zd errno=%d (%s)\n", reg, wn, errno,
                 strerror(errno));
    return false;
  }
  ssize_t rn = read(fd_, &value, 1);
  if (rn != 1) {
    std::fprintf(stderr, "readReg(0x%02x) read failed: n=%zd errno=%d (%s)\n", reg, rn, errno,
                 strerror(errno));
    return false;
  }
  return true;
}

bool Tca8418Keyboard::start(const std::string& i2c_device, std::uint8_t address) {
  fd_ = open(i2c_device.c_str(), O_RDWR);
  if (fd_ < 0) return false;
  if (ioctl(fd_, I2C_SLAVE, address) < 0) return false;

  // Confirm the chip actually answers before committing to init writes -
  // a wrong/absent address should fail start() cleanly rather than send
  // writes into the void.
  std::uint8_t probe = 0;
  if (!readReg(kRegKeyLckEc, probe)) return false;

  bool ok = true;
  ok &= writeReg(kRegKpGpio1, kMatrixGpio1);
  ok &= writeReg(kRegKpGpio2, kMatrixGpio2);
  ok &= writeReg(kRegKpGpio3, kMatrixGpio3);
  // The kernel driver writes the same matrix-enable mask into the
  // DEBOUNCE_DIS registers, which - going by the register's literal name -
  // disables debouncing on every active row/column. On real hardware that
  // produced exactly the symptom debounce disable would predict: a flood
  // of repeated identical events (chatter) on one matrix cell rather than
  // the kernel driver's commented intent of "enable column debouncing".
  // Leave debounce enabled (0x00 = disabled-mask clear) instead.
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
  if (fd_ < 0) return;
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

}  // namespace reconclave
