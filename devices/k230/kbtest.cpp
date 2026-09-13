// Standalone test tool for the TCA8418 keyboard driver - prints raw
// row/col/press-release events so the real matrix wiring can be verified
// against actual key presses before any keymap is built on top of it. Not
// part of the Reconclave app itself.
//
// Usage: kbtest [address] [chip] [scl_offset] [sda_offset] [--raw]
// Defaults match this board's confirmed bit-banged bus (see
// tca8418_keyboard.h's file header for how that was confirmed).
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "src/tca8418_keyboard.h"

namespace {
volatile std::sig_atomic_t g_running = 1;
void handleSignal(int) { g_running = 0; }
}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGINT, handleSignal);
  std::signal(SIGTERM, handleSignal);

  int address = argc > 1 ? std::stoi(argv[1], nullptr, 0) : 0x34;
  const char* chip_path = argc > 2 ? argv[2] : "/dev/gpiochip1";
  unsigned scl_offset = argc > 3 ? static_cast<unsigned>(std::stoul(argv[3])) : 14u;
  unsigned sda_offset = argc > 4 ? static_cast<unsigned>(std::stoul(argv[4])) : 15u;
  bool raw = argc > 5 && std::string(argv[5]) == "--raw";
  bool battery = argc > 5 && std::string(argv[5]) == "--battery";
  bool charger = argc > 5 && std::string(argv[5]) == "--charger";

  reconclave::Tca8418Keyboard keyboard;
  if (!keyboard.start(static_cast<std::uint8_t>(address), chip_path, scl_offset, sda_offset)) {
    std::fprintf(stderr, "failed to start keyboard on %s (scl=%u sda=%u) @ 0x%02x\n", chip_path,
                 scl_offset, sda_offset, address);
    return 1;
  }
  if (battery) {
    const std::pair<std::uint8_t, const char*> registers[] = {
        {0x06, "temperature"}, {0x08, "voltage"}, {0x0A, "status"},
        {0x0C, "current"}, {0x10, "remaining"}, {0x12, "full"},
        {0x2C, "soc"}, {0x2E, "health"}, {0x3A, "operation"}};
    for (const auto& item : registers) {
      std::uint16_t value = 0;
      const bool ok = keyboard.readWord(0x55, item.first, value);
      std::printf("%-12s reg=0x%02x ok=%d value=0x%04x unsigned=%u signed=%d\n",
                  item.second, item.first, ok, value, value, static_cast<std::int16_t>(value));
    }
    return 0;
  }
  if (charger) {
    for (std::uint8_t reg : {std::uint8_t{0x0B}, std::uint8_t{0x0C},
                             std::uint8_t{0x0E}, std::uint8_t{0x0F},
                             std::uint8_t{0x11}, std::uint8_t{0x12},
                             std::uint8_t{0x14}}) {
      std::uint8_t value = 0;
      const bool ok = keyboard.readByte(0x6B, reg, value);
      std::printf("reg=0x%02x ok=%d value=0x%02x\n", reg, ok, value);
    }
    return 0;
  }
  std::printf("listening on %s (scl=%u sda=%u) @ 0x%02x - press keys, Ctrl-C to stop%s\n",
              chip_path, scl_offset, sda_offset, address, raw ? " (raw mode)" : "");
  std::fflush(stdout);

  while (g_running) {
    if (raw) {
      std::uint8_t count = 0xFF, event_byte = 0xFF, int_stat = 0xFF;
      bool count_ok = keyboard.debugReadReg(0x03, count);
      bool stat_ok = keyboard.debugReadReg(0x02, int_stat);
      std::printf("count_ok=%d count=0x%02x stat_ok=%d int_stat=0x%02x", count_ok, count, stat_ok,
                  int_stat);
      if (count_ok && (count & 0x0F) != 0) {
        bool event_ok = keyboard.debugReadReg(0x04, event_byte);
        std::printf(" event_ok=%d event=0x%02x", event_ok, event_byte);
      }
      std::printf("\n");
      std::fflush(stdout);
      usleep(500000);
      continue;
    }
    keyboard.poll([](const reconclave::KeyEvent& event) {
      std::printf("row=%d col=%d %s\n", event.row, event.col, event.pressed ? "down" : "up");
      std::fflush(stdout);
    });
    usleep(30000);
  }
  return 0;
}
