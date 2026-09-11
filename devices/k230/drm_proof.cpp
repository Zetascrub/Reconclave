// UI milestone 1: DRM + LVGL + touch proof.
//
// Not part of the Reconclave app itself yet - a standalone verification
// tool, same pattern as kbtest.cpp/wifitest.cpp. Opens /dev/dri/card0
// through LVGL's own official Linux DRM driver, renders one static screen,
// and wires up the touchscreen (/dev/input/event1, confirmed working) so a
// touch produces visible feedback. Links against the device's actual
// liblvgl.so/liblvgl_linux.so/libdrm.so rather than a self-built LVGL -
// see devices/k230/README.md and devices/k230/fetch-device-sysroot.sh.
//
// Deliberately does not attempt to match k230_phone_ui's 270-degree
// rotation convention yet (native panel orientation is whatever
// lv_linux_drm_create() picks as the preferred mode) - proving the
// pipeline renders and reads touch at all comes first.

#define LV_USE_LINUX_DRM 1
#define LV_USE_EVDEV 1

#include "lvgl/lvgl.h"
#include "lvgl/src/drivers/display/drm/lv_linux_drm.h"
#include "lvgl/src/drivers/evdev/lv_evdev.h"

#include <unistd.h>

#include <csignal>
#include <cstdio>

namespace {
volatile sig_atomic_t g_running = 1;
void handleSignal(int) { g_running = 0; }

void onButtonClicked(lv_event_t* event) {
  auto* button = static_cast<lv_obj_t*>(lv_event_get_target(event));
  static bool toggled = false;
  toggled = !toggled;
  lv_obj_set_style_bg_color(
      button, toggled ? lv_palette_main(LV_PALETTE_GREEN) : lv_palette_main(LV_PALETTE_CYAN), 0);
  std::printf("touch: button toggled -> %s\n", toggled ? "green" : "cyan");
  std::fflush(stdout);
}
}  // namespace

int main() {
  std::signal(SIGINT, handleSignal);
  std::signal(SIGTERM, handleSignal);

  lv_init();

  lv_display_t* display = lv_linux_drm_create();
  if (display == nullptr) {
    std::fprintf(stderr, "lv_linux_drm_create() failed\n");
    return 1;
  }
  if (lv_linux_drm_set_file(display, "/dev/dri/card0", -1) != LV_RESULT_OK) {
    std::fprintf(stderr, "lv_linux_drm_set_file() failed\n");
    return 1;
  }
  std::printf("DRM display up: %ldx%ld\n", static_cast<long>(lv_display_get_horizontal_resolution(display)),
              static_cast<long>(lv_display_get_vertical_resolution(display)));

  lv_indev_t* touch = lv_evdev_create(LV_INDEV_TYPE_POINTER, "/dev/input/event1");
  if (touch == nullptr) {
    std::fprintf(stderr, "lv_evdev_create() failed - continuing without touch\n");
  }

  lv_obj_t* screen = lv_screen_active();
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x0e222e), 0);  // Zeta Mascot palette base.

  lv_obj_t* title = lv_label_create(screen);
  lv_label_set_text(title, "RECONCLAVE");
  lv_obj_set_style_text_color(title, lv_color_hex(0xfff2d7), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

  lv_obj_t* button = lv_button_create(screen);
  lv_obj_set_size(button, 200, 80);
  lv_obj_center(button);
  lv_obj_set_style_bg_color(button, lv_palette_main(LV_PALETTE_CYAN), 0);
  lv_obj_add_event_cb(button, onButtonClicked, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* button_label = lv_label_create(button);
  lv_label_set_text(button_label, "Touch me");
  lv_obj_center(button_label);

  std::printf("running - touch the button, Ctrl-C to stop\n");
  std::fflush(stdout);

  while (g_running) {
    lv_timer_handler();
    usleep(5000);
  }
  return 0;
}
