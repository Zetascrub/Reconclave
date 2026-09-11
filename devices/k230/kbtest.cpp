// Standalone test tool for the TCA8418 keyboard driver - prints raw
// row/col/press-release events so the real matrix wiring can be verified
// against actual key presses before any keymap is built on top of it. Not
// part of the Reconclave app itself.
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <string>

#include "src/tca8418_keyboard.h"

namespace {
volatile std::sig_atomic_t g_running = 1;
void handleSignal(int) { g_running = 0; }
}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGINT, handleSignal);
  std::signal(SIGTERM, handleSignal);

  const char* device = argc > 1 ? argv[1] : "/dev/i2c-0";
  int address = argc > 2 ? std::stoi(argv[2], nullptr, 0) : 0x37;

  reconclave::Tca8418Keyboard keyboard;
  if (!keyboard.start(device, static_cast<std::uint8_t>(address))) {
    std::fprintf(stderr, "failed to start keyboard on %s @ 0x%02x\n", device, address);
    return 1;
  }
  bool raw = argc > 3 && std::string(argv[3]) == "--raw";
  std::printf("listening on %s @ 0x%02x - press keys, Ctrl-C to stop%s\n", device, address,
              raw ? " (raw mode)" : "");
  std::fflush(stdout);

  while (g_running) {
    if (raw) {
      std::uint8_t count = 0xFF, event_byte = 0xFF, int_stat = 0xFF;
      bool count_ok = keyboard.debugReadReg(0x03, count);
      bool stat_ok = keyboard.debugReadReg(0x02, int_stat);
      std::printf("count_ok=%d count=0x%02x stat_ok=%d int_stat=0x%02x", count_ok, count, stat_ok,
                  int_stat);
      if (count_ok && (count & 0x7) != 0) {
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
