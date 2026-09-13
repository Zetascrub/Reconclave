// K230 touch UI - the §18 mockup (Reconclave_Design_Document_v0.1.md),
// built on the proven DRM+LVGL+touch pipeline (devices/k230/drm_proof.cpp)
// and the Wi-Fi scanner (src/wifi_scanner.cpp). Standalone for now, same
// as drm_proof.cpp/wifitest.cpp/kbtest.cpp - not yet merged with the
// network-serving app (main.cpp) into one process; see
// devices/k230/README.md for why and what merging them would take.
//
// Capability-aware, matching the design doc's principle that unavailable
// operations must not affect unrelated functions. Every dashboard area is
// navigable; actions that still require scope enforcement or a protected
// camera pipeline are explained and kept locked on their detail screens.

#define LV_USE_LINUX_DRM 1
#define LV_USE_EVDEV 1

#include "lvgl/lvgl.h"
#include "lvgl/src/misc/cache/instance/lv_image_cache.h"
#include "lvgl/src/drivers/display/drm/lv_linux_drm.h"
#include "lvgl/src/drivers/evdev/lv_evdev.h"
#include <gpiod.h>

extern "C" {
// Not declared in the upstream LVGL header above: Canaan/LILYGO's own
// liblvgl.so has a natively-built, working lv_linux_drm_set_rotation
// (confirmed via `nm -D` - it's what k230_phone_ui itself calls to
// implement its Display > Rotation setting), just missing from the
// public header we're building against. A prior attempt to reimplement
// this from scratch against upstream LVGL (see
// devices/k230/third_party/lvgl_drm_rotate/, kept for its documented
// history) had a still-unresolved pixel-mapping bug after several
// iterations; this links against the vendor's proven implementation
// instead. Called before lv_linux_drm_set_file() below - confirmed
// correct on real hardware: landscape orientation and touch both work.
void lv_linux_drm_set_rotation(lv_display_t* disp, int rotation);

// Same situation, different feature: our own lv_conf.h has LV_USE_SYSMON
// off, so the perf-monitor overlay it draws (FPS/render-time, visible by
// default since LV_USE_PERF_MONITOR is otherwise on) has no declared API
// for us to hide it with - but the vendor's liblvgl.so has LV_USE_SYSMON
// on and exports this symbol (confirmed via `nm -D`), so it's safe to
// call.
}

#include <sys/utsname.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <unistd.h>

#include <cstdio>
#include <csignal>
#include <cerrno>
#include <cstdlib>
#include <ctime>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "src/ui_shell.h"
#include "src/network_inventory.h"
#include "src/gnss_receiver.h"
#include "src/wifi_scanner.h"
#include "src/evidence_store.h"
#include "src/host_knowledge.h"
#include "src/json.h"
#include "src/camera_capture.h"
#include "src/tca8418_keyboard.h"

namespace {

constexpr char kDeviceId[] = "rc-k230-poc";
constexpr char kFirmware[] = "reconclave-k230-0.2.0-alpha4";
constexpr char kEvidenceDir[] = "/root/reconclave/evidence";
constexpr char kSessionPath[] = "/root/reconclave/session.conf";
constexpr char kHostKnowledgePath[] = "/root/reconclave/hosts.json";
constexpr char kLocalCommandPath[] = "/run/reconclave/local-command";
constexpr char kUiCommandPath[] = "/run/reconclave/ui-command";
// The GC2093 sensor's actual ISP capture output - confirmed via
// `v4l2-ctl --list-formats-ext` reporting "camera: ok" and a real (not
// black/garbage) test capture, as opposed to /dev/video0 (a memory-to-memory
// codec node, not a camera) or /dev/video1 (same ISP pipeline, unverified).
constexpr char kCameraDevice[] = "/dev/video2";
// The keyboard base exposes an nRF9151 UART, but the shipped modem firmware
// does not currently accept the GNSS configuration required by Reconclave.
// Keep the UI honest until that module is updated independently.
constexpr bool kGnssAvailable = false;

volatile sig_atomic_t g_running = 1;
void handleSignal(int) { g_running = 0; }

std::vector<reconclave::WifiObservation> g_last_wifi_scan;
lv_obj_t* g_wifi_list = nullptr;
lv_obj_t* g_wifi_status = nullptr;
lv_obj_t* g_evidence_status = nullptr;
lv_obj_t* g_evidence_summary = nullptr;
lv_obj_t* g_network_details = nullptr;
lv_obj_t* g_network_summary = nullptr;
lv_obj_t* g_recon_details = nullptr;
lv_obj_t* g_vision_details = nullptr;
lv_obj_t* g_vision_status = nullptr;
lv_obj_t* g_vision_screen = nullptr;
lv_obj_t* g_settings_trust_status = nullptr;
lv_obj_t* g_battery_details = nullptr;
lv_obj_t* g_screenshot_status = nullptr;
lv_obj_t* g_screenshot_action_status = nullptr;
pid_t g_screenshot_pid = -1;
lv_obj_t* g_vision_preview = nullptr;
lv_obj_t* g_vision_preview_hint = nullptr;
lv_obj_t* g_vision_preview_button = nullptr;
lv_obj_t* g_vision_preview_button_label = nullptr;
// Created once in buildVisionScreen(), paused/resumed (never deleted) as
// VISION is entered/left - see the screen's LV_EVENT_SCREEN_LOADED/
// LV_EVENT_SCREEN_UNLOADED handlers.
lv_timer_t* g_vision_preview_timer = nullptr;
lv_obj_t* g_photo_viewer_screen = nullptr;
lv_obj_t* g_photo_viewer_image = nullptr;
lv_obj_t* g_photo_viewer_caption = nullptr;

// Wi-Fi *client* settings (join a network) - distinct from the WIRELESS
// home tile, which is a recon survey tool. See README's Settings section
// for why these need to be different screens, not one linking to the
// other.
std::vector<reconclave::WifiObservation> g_wifisetup_scan;
lv_obj_t* g_wifisetup_screen = nullptr;
lv_obj_t* g_wifisetup_status = nullptr;
lv_obj_t* g_wifisetup_list = nullptr;
lv_obj_t* g_wifi_password_screen = nullptr;
lv_obj_t* g_wifi_password_title = nullptr;
lv_obj_t* g_wifi_password_field = nullptr;
lv_obj_t* g_wifi_password_status = nullptr;
lv_obj_t* g_wifi_password_connect_button = nullptr;
std::string g_wifi_pending_ssid;

// One background op at a time (connect or disconnect), same
// worker-thread-plus-generation-counter pattern as the camera preview
// (previewWorkerLoop/checkPreviewWorker) - a real Wi-Fi association can
// take several seconds and must not block the UI thread. Confirmed safe
// against a real access point (connect, then a clean `ifdown wlan0`
// teardown, twice in a row) before building this - see README: an
// earlier device hang traced to killing a wpa_supplicant that was stuck
// retrying against a *nonexistent* network, not to Wi-Fi cycling itself,
// which is why disconnect always goes through `ifdown wlan0` (its own
// post-down hook stops wpa_supplicant cleanly) rather than a raw
// `killall wpa_supplicant`.
std::atomic<bool> g_wifi_op_running{false};
std::thread g_wifi_op_thread;
std::mutex g_wifi_op_mutex;
bool g_wifi_op_ok = false;
std::string g_wifi_op_message;
std::atomic<std::uint64_t> g_wifi_op_generation{0};
std::uint64_t g_wifi_op_applied_generation = 0;
// Whatever was most recently captured (either a throwaway viewfinder frame
// or a saved evidence photo) - the VISION screen's preview thumbnail and
// the full-screen viewer both just show this, so "preview" and "capture"
// share one code path instead of tracking two separate images.
std::string g_last_photo_path;
constexpr char kPreviewPaths[][27] = {
    "/tmp/vision-preview-0.jpg",
    "/tmp/vision-preview-1.jpg",
};
// Fixed capture resolution (see camera_capture.cpp's -video_size), used to
// compute a "contain" scale for the viewer/thumbnail without needing to
// query the decoder for image dimensions first.
constexpr int kCaptureWidth = 1920;
constexpr int kCaptureHeight = 1080;
lv_obj_t* g_device_details = nullptr;
lv_obj_t* g_device_services = nullptr;
lv_obj_t* g_gnss_details = nullptr;
reconclave::GnssReceiver g_gnss;
std::string g_project_id{"UNASSIGNED"};
std::string g_engagement_id{"UNASSIGNED"};
std::string g_operator_id{"LOCAL"};
lv_obj_t* g_session_status = nullptr;
lv_obj_t* g_node_details = nullptr;
lv_obj_t* g_node_trust = nullptr;
lv_obj_t* g_node_job = nullptr;
lv_obj_t* g_recon_action_status = nullptr;
bool g_lan_survey_armed = false;
std::time_t g_lan_survey_armed_at = 0;
std::string g_backlight_path;
int g_backlight_max = 255;
lv_obj_t* g_home_screen = nullptr;
lv_obj_t* g_home_context = nullptr;
lv_obj_t* g_home_readiness = nullptr;
lv_obj_t* g_capture_status = nullptr;
lv_obj_t* g_capture_stop_button = nullptr;
pid_t g_capture_pid = -1;
std::string g_capture_preset;
lv_obj_t* g_nmap_output = nullptr;
lv_obj_t* g_nmap_scope = nullptr;
pid_t g_nmap_pid = -1;
reconclave::Tca8418Keyboard g_keyboard;
bool g_keyboard_ready = false;
bool g_keyboard_shift = false;
bool g_keyboard_caps = false;
std::vector<std::pair<std::string, lv_obj_t*>> g_ui_screens;

struct QueuedKey {
  std::uint32_t key;
  lv_indev_state_t state;
};
std::deque<QueuedKey> g_keyboard_queue;

struct BatteryState {
  bool valid = false;
  bool stale = false;
  int consecutive_failures = 0;
  bool estimated = false;
  int percent = 0;
  int voltage_mv = 0;
  int current_ma = 0;
  int remaining_mah = 0;
  int full_mah = 0;
  int health_percent = 0;
  double temperature_c = 0.0;
  bool usb_present = false;
  bool charging = false;
  bool charge_done = false;
  int vbus_mv = 0;
  int charge_current_ma = 0;
};
BatteryState g_battery;
gpiod_chip* g_nrf9151_gpio_chip = nullptr;
gpiod_line_request* g_nrf9151_enable_request = nullptr;

bool enableNrf9151() {
  if (g_nrf9151_enable_request != nullptr) return true;
  g_nrf9151_gpio_chip = gpiod_chip_open("/dev/gpiochip0");
  if (g_nrf9151_gpio_chip == nullptr) return false;
  gpiod_line_settings* settings = gpiod_line_settings_new();
  gpiod_line_config* lines = gpiod_line_config_new();
  gpiod_request_config* request = gpiod_request_config_new();
  constexpr unsigned int kEnableOffset = 2;
  if (settings == nullptr || lines == nullptr || request == nullptr) return false;
  gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT);
  gpiod_line_settings_set_output_value(settings, GPIOD_LINE_VALUE_ACTIVE);
  gpiod_line_config_add_line_settings(lines, &kEnableOffset, 1, settings);
  gpiod_request_config_set_consumer(request, "reconclave-nrf9151");
  g_nrf9151_enable_request = gpiod_chip_request_lines(g_nrf9151_gpio_chip, request, lines);
  gpiod_line_settings_free(settings);
  gpiod_line_config_free(lines);
  gpiod_request_config_free(request);
  if (g_nrf9151_enable_request == nullptr) return false;
  usleep(600000);
  return true;
}

int estimateLithiumPercent(int millivolts) {
  struct Point { int mv; int percent; };
  static constexpr Point curve[] = {
      {3300, 0}, {3500, 5}, {3700, 18}, {3800, 35},
      {3900, 55}, {4000, 75}, {4100, 90}, {4200, 100}};
  if (millivolts <= curve[0].mv) return 0;
  for (std::size_t i = 1; i < sizeof(curve) / sizeof(curve[0]); ++i) {
    if (millivolts <= curve[i].mv) {
      const int span = curve[i].mv - curve[i - 1].mv;
      return curve[i - 1].percent +
          (millivolts - curve[i - 1].mv) *
          (curve[i].percent - curve[i - 1].percent) / span;
    }
  }
  return 100;
}

void writeBatteryRuntimeStatus() {
  const std::string temporary = "/run/reconclave/battery-status.tmp";
  std::ofstream out(temporary, std::ios::trunc);
  if (!out) return;
  out << "PRESENT " << (g_battery.valid ? "yes" : "no") << '\n';
  if (g_battery.valid) {
    out << "STALE " << (g_battery.stale ? "yes" : "no") << '\n'
        << "READ_FAILURES " << g_battery.consecutive_failures << '\n'
        << "PERCENT " << g_battery.percent << '\n'
        << "ESTIMATED " << (g_battery.estimated ? "yes" : "no") << '\n'
        << "VOLTAGE_MV " << g_battery.voltage_mv << '\n'
        << "CURRENT_MA " << g_battery.current_ma << '\n'
        << "TEMPERATURE_C " << std::fixed << std::setprecision(1)
        << g_battery.temperature_c << '\n'
        << "REMAINING_MAH " << g_battery.remaining_mah << '\n'
        << "FULL_MAH " << g_battery.full_mah << '\n'
        << "HEALTH_PERCENT " << g_battery.health_percent << '\n';
    out << "USB_PRESENT " << (g_battery.usb_present ? "yes" : "no") << '\n'
        << "CHARGING " << (g_battery.charging ? "yes" : "no") << '\n'
        << "CHARGE_DONE " << (g_battery.charge_done ? "yes" : "no") << '\n'
        << "VBUS_MV " << g_battery.vbus_mv << '\n'
        << "CHARGE_CURRENT_MA " << g_battery.charge_current_ma << '\n';
  }
  out.close();
  if (out.good()) rename(temporary.c_str(), "/run/reconclave/battery-status");
}

void writeGnssRuntimeStatus() {
  const std::string temporary = "/run/reconclave/gnss-status.tmp";
  std::ofstream out(temporary, std::ios::trunc);
  if (!out) return;
  const auto& fix = g_gnss.fix();
  out << "RUNNING " << (g_gnss.running() ? "yes" : "no") << '\n'
      << "STATUS " << g_gnss.status() << '\n'
      << "FIX " << (fix.valid ? "yes" : "no") << '\n'
      << "SATELLITES " << fix.satellites << '\n';
  out << "NMEA_COUNT " << g_gnss.nmeaCount() << '\n'
      << "LAST_RESPONSE " << g_gnss.lastResponse() << '\n'
      << "MODEM_INFO " << g_gnss.modemInfo() << '\n'
      << "API_INFO " << g_gnss.apiInfo() << '\n';
  if (fix.valid) {
    out << std::fixed << std::setprecision(7)
        << "LATITUDE " << fix.latitude << '\n'
        << "LONGITUDE " << fix.longitude << '\n'
        << std::setprecision(1) << "ALTITUDE_M " << fix.altitude_m << '\n';
  }
  out.close();
  if (out.good()) rename(temporary.c_str(), "/run/reconclave/gnss-status");
}

// Host drill-down (RECON's host list -> Host Detail) and the Findings list.
// Only one Host Detail screen is built (reused for whichever host was last
// tapped, same "one detail screen, repopulated" approach the rest of this
// file doesn't otherwise need since every other screen's content is fixed
// at build time) - g_host_detail_address tracks which host it currently
// shows, since the "Re-check ports" button reads it at click time rather
// than at screen-build time.
//
// RECON/Findings/Host Detail form a real hierarchy (unlike every other
// screen here, which is one tap from home and back always returns to
// home) - g_recon_screen/g_findings_screen and g_host_detail_return let
// each back button resolve its *actual* parent at click time instead of
// always jumping to the top-level tile grid.
lv_obj_t* g_recon_host_list = nullptr;
lv_obj_t* g_findings_list = nullptr;
lv_obj_t* g_findings_status = nullptr;
lv_obj_t* g_recon_screen = nullptr;
lv_obj_t* g_findings_screen = nullptr;
lv_obj_t* g_findings_return = nullptr;
lv_obj_t* g_host_detail_screen = nullptr;
lv_obj_t* g_host_detail_title = nullptr;
lv_obj_t* g_host_detail_body = nullptr;
lv_obj_t* g_host_detail_status = nullptr;
lv_obj_t* g_host_detail_recheck_button = nullptr;
lv_obj_t* g_host_detail_return = nullptr;
std::string g_host_detail_address;

void refreshEvidenceSummary();
void refreshReconHostList();
void refreshFindingsList();
void populateHostDetail(const std::string& address);
void showHostDetail(const std::string& address, lv_obj_t* return_screen);
// Builds a "< back" button identical in style to reconclave::ui::addBackButton,
// but resolving its destination at click time via `target` rather than a
// fixed screen fixed at construction - for the RECON -> Findings -> Host
// Detail chain, where "back" means "the screen that opened this one", not
// always the top-level home grid.
void addDynamicBackButton(lv_obj_t* screen, lv_obj_t** target, lv_obj_t* fallback);
std::string trustStatusLine();
void showLastPhoto(const std::string& path, lv_obj_t* thumbnail_target);
void openPhotoViewer();
void refreshWifiSetupList();
void showWifiPasswordScreen(const std::string& ssid);
std::string readFirstLine(const std::string& path);
std::string readDeviceInfo();
void refreshDeviceDetails();
int arpNeighbourCount();

std::string attachedIpv4Network() {
  ifaddrs* addrs = nullptr;
  if (getifaddrs(&addrs) != 0) return {};
  std::string result;
  for (ifaddrs* it = addrs; it != nullptr; it = it->ifa_next) {
    if (!it->ifa_addr || !it->ifa_netmask || it->ifa_addr->sa_family != AF_INET ||
        (it->ifa_flags & IFF_LOOPBACK) != 0 || (it->ifa_flags & IFF_UP) == 0) continue;
    const auto address = ntohl(reinterpret_cast<sockaddr_in*>(it->ifa_addr)->sin_addr.s_addr);
    const auto mask = ntohl(reinterpret_cast<sockaddr_in*>(it->ifa_netmask)->sin_addr.s_addr);
    unsigned prefix = 0;
    for (std::uint32_t bit = 0x80000000U; bit != 0 && (mask & bit) != 0; bit >>= 1) ++prefix;
    if (prefix < 24) prefix = 24;
    const std::uint32_t bounded_mask = 0xffffffffU << (32 - prefix);
    in_addr network_address{htonl(address & bounded_mask)};
    char text[INET_ADDRSTRLEN]{};
    if (inet_ntop(AF_INET, &network_address, text, sizeof(text))) {
      result = std::string(text) + "/" + std::to_string(prefix);
      break;
    }
  }
  freeifaddrs(addrs);
  return result;
}

bool sendLocalCommand(const std::string& command) {
  mkdir("/run/reconclave", 0755);
  const std::string temporary = std::string(kLocalCommandPath) + ".tmp";
  std::ofstream output(temporary, std::ios::trunc);
  if (!output) return false;
  output << command << '\n';
  output.close();
  chmod(temporary.c_str(), 0600);
  return output.good() && rename(temporary.c_str(), kLocalCommandPath) == 0;
}

void requestLanSurvey() {
  const std::time_t now = std::time(nullptr);
  if (!g_lan_survey_armed || now - g_lan_survey_armed_at > 5) {
    g_lan_survey_armed = true;
    g_lan_survey_armed_at = now;
    lv_label_set_text(g_recon_action_status,
                      "Assessment traffic: tap Survey LAN again within 5 seconds to confirm.");
    return;
  }
  g_lan_survey_armed = false;
  const std::string network = attachedIpv4Network();
  if (network.empty()) {
    lv_label_set_text(g_recon_action_status, "No active IPv4 network is available.");
  } else if (sendLocalCommand("DISCOVERY " + network)) {
    const std::string message = "Survey started: " + network + "  |  progress in Node & Jobs";
    lv_label_set_text(g_recon_action_status, message.c_str());
  } else {
    lv_label_set_text(g_recon_action_status, "Unable to contact the local node service.");
  }
}

void cancelLanSurvey() {
  g_lan_survey_armed = false;
  lv_label_set_text(g_recon_action_status,
                    sendLocalCommand("CANCEL") ? "Cancellation requested." : "Unable to contact node service.");
}

std::string hostKnowledgeSummary() {
  reconclave::HostKnowledge knowledge(kHostKnowledgePath);
  std::string error;
  if (!knowledge.load(error)) return "HOST KNOWLEDGE\nDatabase error: " + error;
  const auto hosts = knowledge.snapshot();
  std::size_t services = 0;
  std::uint64_t changes = 0;
  for (const auto& host : hosts) {
    services += host.open_ports.size();
    changes += host.changes;
  }
  return "HOST KNOWLEDGE\n" + std::to_string(hosts.size()) + " hosts  |  " +
         std::to_string(services) + " services  |  " +
         std::to_string(knowledge.attentionCount()) + " review\n" +
         std::to_string(changes) + " observed state changes";
}

// Summary line only - the per-host breakdown lives in the tappable host
// list built by refreshReconHostList() instead of being dumped as text
// here, so a host can actually be drilled into (see showHostDetail()).
std::string reconDashboardText() {
  reconclave::HostKnowledge knowledge(kHostKnowledgePath);
  std::string error;
  knowledge.load(error);
  const auto hosts = knowledge.snapshot();
  std::ostringstream out;
  out << "TARGET NETWORK   " << (attachedIpv4Network().empty() ? "offline" : attachedIpv4Network())
      << "     PASSIVE NEIGHBOURS   " << arpNeighbourCount() << "\n";
  out << "KNOWN HOSTS      " << hosts.size() << "     REVIEW ITEMS         "
      << knowledge.attentionCount();
  return out.str();
}

std::string safeSessionValue(std::string value, const char* fallback) {
  value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char c) {
    return c < 0x20 || c == '=' || c == ',';
  }), value.end());
  if (value.size() > 48) value.resize(48);
  return value.empty() ? fallback : value;
}

void loadSession() {
  std::ifstream input(kSessionPath);
  std::string line;
  while (std::getline(input, line)) {
    const auto separator = line.find('=');
    if (separator == std::string::npos) continue;
    const std::string key = line.substr(0, separator);
    const std::string value = line.substr(separator + 1);
    if (key == "project") g_project_id = safeSessionValue(value, "UNASSIGNED");
    else if (key == "engagement") g_engagement_id = safeSessionValue(value, "UNASSIGNED");
    else if (key == "operator") g_operator_id = safeSessionValue(value, "LOCAL");
  }
}

bool saveSession() {
  if ((mkdir("/root/reconclave", 0755) != 0 && errno != EEXIST)) return false;
  const std::string temporary = std::string(kSessionPath) + ".tmp";
  std::ofstream output(temporary, std::ios::trunc);
  if (!output) return false;
  output << "project=" << g_project_id << "\nengagement=" << g_engagement_id
         << "\noperator=" << g_operator_id << "\n";
  output.close();
  return output.good() && rename(temporary.c_str(), kSessionPath) == 0;
}

bool findBacklight() {
  DIR* dir = opendir("/sys/class/backlight");
  if (dir == nullptr) return false;
  while (dirent* entry = readdir(dir)) {
    if (entry->d_name[0] == '.') continue;
    std::string base = std::string("/sys/class/backlight/") + entry->d_name;
    std::string maximum = readFirstLine(base + "/max_brightness");
    if (maximum.empty()) continue;
    g_backlight_path = base + "/brightness";
    g_backlight_max = std::max(1, std::atoi(maximum.c_str()));
    closedir(dir);
    return true;
  }
  closedir(dir);
  return false;
}

std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

std::string readFirstLine(const std::string& path) {
  std::ifstream file(path);
  std::string value;
  std::getline(file, value);
  return trim(value);
}

std::string readTextFile(const std::string& path) {
  std::ifstream input(path);
  if (!input) return {};
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string formatBytes(uint64_t bytes) {
  const char* units[] = {"B", "KB", "MB", "GB"};
  double value = static_cast<double>(bytes);
  size_t unit = 0;
  while (value >= 1024.0 && unit < 3) {
    value /= 1024.0;
    ++unit;
  }
  std::ostringstream out;
  out << std::fixed << std::setprecision(unit > 1 ? 1 : 0) << value << " " << units[unit];
  return out.str();
}

void refreshBatteryState() {
  static int last_reported_percent = -2;
  if (!g_keyboard_ready) { g_battery.valid = false; return; }
  constexpr std::uint8_t kGaugeAddress = 0x55;
  std::uint16_t voltage = 0, soc = 0, current = 0, temperature = 0;
  std::uint16_t remaining = 0, full = 0, health = 0;
  if (!g_keyboard.readWord(kGaugeAddress, 0x08, voltage) ||
      voltage <= 2500 || voltage >= 6000) {
    ++g_battery.consecutive_failures;
    g_battery.stale = g_battery.valid;
    // Software-I2C occasionally loses a transaction while the keyboard and
    // charger share the bus. Keep the last validated sample instead of making
    // the status bar oscillate. Only call it absent if it never worked.
    if (!g_battery.valid && last_reported_percent != -1) {
      std::fprintf(stderr, "reconclave k230 battery: BQ27220 not detected\n");
      last_reported_percent = -1;
    }
    return;
  }
  const bool soc_valid = g_keyboard.readWord(kGaugeAddress, 0x2C, soc) && soc <= 100;
  const bool current_valid = g_keyboard.readWord(kGaugeAddress, 0x0C, current);
  const bool temperature_valid = g_keyboard.readWord(kGaugeAddress, 0x06, temperature) &&
                                 temperature >= 2500 && temperature <= 3500;
  const bool remaining_valid = g_keyboard.readWord(kGaugeAddress, 0x10, remaining) && remaining < 30000;
  const bool full_valid = g_keyboard.readWord(kGaugeAddress, 0x12, full) && full < 30000;
  const bool health_valid = g_keyboard.readWord(kGaugeAddress, 0x2E, health) && health <= 100;
  std::uint8_t charger_status = 0, vbus_adc = 0, charge_current_adc = 0;
  // The charger shares the software-I2C bus with the keyboard and gauge.
  // Require a repeated value so a single corrupt byte cannot report phantom
  // USB power or an impossible VBUS voltage.
  auto stableChargerByte = [](std::uint8_t reg, std::uint8_t& value) {
    std::uint8_t samples[3]{};
    bool valid[3]{};
    for (int i = 0; i < 3; ++i) valid[i] = g_keyboard.readByte(0x6B, reg, samples[i]);
    if (valid[0] && valid[1] && samples[0] == samples[1]) { value = samples[0]; return true; }
    if (valid[0] && valid[2] && samples[0] == samples[2]) { value = samples[0]; return true; }
    if (valid[1] && valid[2] && samples[1] == samples[2]) { value = samples[1]; return true; }
    return false;
  };
  const bool charger_status_valid = stableChargerByte(0x0B, charger_status);
  const bool vbus_valid = stableChargerByte(0x11, vbus_adc);
  const bool charge_current_valid = stableChargerByte(0x12, charge_current_adc);
  g_battery.valid = true;
  g_battery.stale = false;
  g_battery.consecutive_failures = 0;
  g_battery.estimated = !soc_valid;
  g_battery.percent = soc_valid ? static_cast<int>(soc) : estimateLithiumPercent(voltage);
  g_battery.voltage_mv = static_cast<int>(voltage);
  g_battery.current_ma = current_valid ? static_cast<std::int16_t>(current) : 0;
  g_battery.temperature_c = temperature_valid ? temperature * 0.1 - 273.15 : 0.0;
  g_battery.remaining_mah = remaining_valid ? static_cast<int>(remaining) : 0;
  g_battery.full_mah = full_valid ? static_cast<int>(full) : 0;
  g_battery.health_percent = health_valid ? static_cast<int>(health) : 0;
  const int charge_state = charger_status_valid ? ((charger_status >> 3) & 0x03) : 0;
  const int vbus_state = charger_status_valid ? ((charger_status >> 5) & 0x07) : 0;
  g_battery.usb_present = charger_status_valid && (charger_status & 0x04) != 0 &&
      vbus_state != 0 && vbus_state != 7;
  g_battery.charging = g_battery.usb_present && (charge_state == 1 || charge_state == 2);
  g_battery.charge_done = g_battery.usb_present && charge_state == 3;
  g_battery.vbus_mv = g_battery.usb_present && vbus_valid && (vbus_adc & 0x80)
      ? 2600 + (vbus_adc & 0x7F) * 100 : 0;
  g_battery.charge_current_ma = g_battery.usb_present && charge_current_valid
      ? (charge_current_adc & 0x7F) * 50 : 0;
  if (last_reported_percent != g_battery.percent) {
    std::fprintf(stderr, "reconclave k230 battery: %d%% %dmV %dmA %.1fC %d/%dmAh health=%d%%\n",
                 g_battery.percent, g_battery.voltage_mv, g_battery.current_ma,
                 g_battery.temperature_c, g_battery.remaining_mah, g_battery.full_mah,
                 g_battery.health_percent);
    last_reported_percent = g_battery.percent;
  }
}

std::string batteryText() {
  if (!g_battery.valid) return LV_SYMBOL_BATTERY_EMPTY " N/A";
  const char* icon = g_battery.percent >= 85 ? LV_SYMBOL_BATTERY_FULL :
      g_battery.percent >= 60 ? LV_SYMBOL_BATTERY_3 :
      g_battery.percent >= 35 ? LV_SYMBOL_BATTERY_2 :
      g_battery.percent >= 12 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
  return std::string(g_battery.charging ? LV_SYMBOL_CHARGE : icon) + " " +
         std::to_string(g_battery.percent) + "%";
}

std::string batteryDetailsText() {
  if (!g_battery.valid) {
    return "Battery gauge not detected\nCheck the keyboard base power switch and docking latch.";
  }
  std::ostringstream out;
  out << (g_battery.charging ? "Charging" : g_battery.charge_done ? "Full" :
          g_battery.current_ma < 0 ? "On battery" : "Idle")
      << "   " << g_battery.percent << "%"
      << (g_battery.estimated ? " estimated" : "") << "\n"
      << std::fixed << std::setprecision(2) << g_battery.voltage_mv / 1000.0 << " V   "
      << g_battery.current_ma << " mA";
  if (g_battery.stale) out << "   Last reading";
  if (g_battery.temperature_c != 0.0) out << "   " << std::setprecision(1) << g_battery.temperature_c << " C";
  out << "\n";
  if (!g_battery.estimated && g_battery.full_mah > 0) {
    out << g_battery.remaining_mah << " / " << g_battery.full_mah << " mAh";
  } else {
    out << "Gauge learning incomplete";
    if (g_battery.full_mah > 0) out << "   " << g_battery.full_mah << " mAh profile";
  }
  if (g_battery.usb_present) {
    out << "\nUSB power";
    if (g_battery.vbus_mv > 0) out << "   " << std::setprecision(1) << g_battery.vbus_mv / 1000.0 << " V";
    if (g_battery.charging) out << "   " << g_battery.charge_current_ma << " mA charge";
  }
  if (g_battery.health_percent > 0) out << "   Health " << g_battery.health_percent << "%";
  return out.str();
}

struct NetworkState {
  bool connected = false;
  std::string iface = "offline";
  std::string address = "No IPv4 address";
  std::string gateway = "No default route";
};

std::string interfaceIpv4Address(const char* interface_name) {
  ifaddrs* addrs = nullptr;
  if (getifaddrs(&addrs) != 0) return {};
  std::string result;
  for (ifaddrs* it = addrs; it != nullptr; it = it->ifa_next) {
    if (it->ifa_name == nullptr || it->ifa_addr == nullptr ||
        std::strcmp(it->ifa_name, interface_name) != 0 || it->ifa_addr->sa_family != AF_INET) continue;
    char address[INET_ADDRSTRLEN]{};
    const auto* in = reinterpret_cast<sockaddr_in*>(it->ifa_addr);
    if (inet_ntop(AF_INET, &in->sin_addr, address, sizeof(address)) != nullptr) result = address;
    break;
  }
  freeifaddrs(addrs);
  return result;
}

NetworkState networkState() {
  NetworkState state;
  ifaddrs* addresses = nullptr;
  if (getifaddrs(&addresses) == 0) {
    for (ifaddrs* item = addresses; item != nullptr; item = item->ifa_next) {
      if (item->ifa_addr == nullptr || item->ifa_addr->sa_family != AF_INET ||
          (item->ifa_flags & IFF_LOOPBACK) != 0) continue;
      char value[INET_ADDRSTRLEN]{};
      const auto* in = reinterpret_cast<sockaddr_in*>(item->ifa_addr);
      if (inet_ntop(AF_INET, &in->sin_addr, value, sizeof(value)) != nullptr) {
        state.connected = true;
        state.iface = item->ifa_name;
        state.address = value;
        break;
      }
    }
    freeifaddrs(addresses);
  }

  std::ifstream routes("/proc/net/route");
  std::string line;
  std::getline(routes, line);
  while (std::getline(routes, line)) {
    std::istringstream row(line);
    std::string iface, destination, gateway;
    row >> iface >> destination >> gateway;
    if (destination != "00000000" || gateway.size() != 8) continue;
    uint32_t raw = 0;
    std::istringstream(gateway) >> std::hex >> raw;
    in_addr address{raw};
    char value[INET_ADDRSTRLEN]{};
    if (inet_ntop(AF_INET, &address, value, sizeof(value)) != nullptr) {
      state.gateway = value;
    }
    break;
  }
  return state;
}

int arpNeighbourCount() {
  std::vector<reconclave::ArpNeighbour> neighbours;
  std::string error;
  if (!reconclave::readArpSnapshot(neighbours, error)) return 0;
  return static_cast<int>(neighbours.size());
}

std::string arpSummary() {
  std::vector<reconclave::ArpNeighbour> neighbours;
  std::string error;
  if (!reconclave::readArpSnapshot(neighbours, error)) return "Neighbour cache unavailable";
  std::ostringstream out;
  out << neighbours.size() << " passive neighbours observed";
  int shown = 0;
  for (const auto& neighbour : neighbours) {
    if (!neighbour.complete || shown >= 5) continue;
    out << "\n" << neighbour.address << "   " << neighbour.mac << "   " << neighbour.interface;
    ++shown;
  }
  if (neighbours.size() > static_cast<std::size_t>(shown)) {
    out << "\n+" << (neighbours.size() - static_cast<std::size_t>(shown)) << " more cached entries";
  }
  return out.str();
}

bool pathExists(const char* path) { return access(path, F_OK) == 0; }

lv_coord_t contentWidth(lv_obj_t* object) {
  return lv_display_get_horizontal_resolution(lv_obj_get_display(object)) -
         2 * reconclave::ui::kSafeMargin;
}

lv_coord_t contentHeight(lv_obj_t* object) {
  return lv_display_get_vertical_resolution(lv_obj_get_display(object)) -
         reconclave::ui::kContentTop - reconclave::ui::kSafeMargin;
}

int keyboardCode(const reconclave::KeyEvent& event) {
  return event.row * 10 + event.col + 1;
}

std::uint32_t shiftedKeyboardSymbol(int code) {
  switch (code) {
    case 49: return '!'; case 48: return '@'; case 58: return '#';
    case 57: return '$'; case 56: return '%'; case 55: return '^';
    case 54: return '&'; case 53: return '*'; case 52: return '(';
    case 51: return ')'; case 20: return '`'; case 39: return '~';
    case 38: return '-'; case 37: return '+'; case 47: return '=';
    case 46: return '\\'; case 45: return '|'; case 44: return ';';
    case 43: return ':'; case 42: return '"'; case 29: return '~';
    case 28: return '['; case 27: return ']'; case 26: return '{';
    case 36: return '}'; case 35: return ','; case 34: return '`';
    case 33: return '/'; case 32: return '?'; case 25: return '.';
    case 24: return '<'; case 13: return '>';
    default: return 0;
  }
}

std::uint32_t keyboardKeyForCode(int code) {
  switch (code) {
    case 1: return LV_KEY_RIGHT;
    case 2: return LV_KEY_LEFT;
    case 5: case 14: return ' ';
    case 6: return LV_KEY_NEXT;
    case 12: return LV_KEY_DOWN;
    case 21: return LV_KEY_ENTER;
    case 22: return LV_KEY_UP;
    case 40: return LV_KEY_ESC;
    case 41: return LV_KEY_BACKSPACE;
    default: break;
  }
  if (g_keyboard_shift) {
    const auto symbol = shiftedKeyboardSymbol(code);
    if (symbol != 0) return symbol;
  }
  static const std::pair<int, char> numbers[] = {
      {49, '1'}, {48, '2'}, {58, '3'}, {57, '4'}, {56, '5'},
      {55, '6'}, {54, '7'}, {53, '8'}, {52, '9'}, {51, '0'}};
  for (const auto& item : numbers) if (item.first == code) return item.second;
  static const std::pair<int, char> letters[] = {
      {20,'Q'}, {39,'W'}, {38,'E'}, {37,'R'}, {47,'T'}, {46,'Y'}, {45,'U'},
      {44,'I'}, {43,'O'}, {42,'P'}, {29,'A'}, {28,'S'}, {27,'D'}, {26,'F'},
      {36,'G'}, {35,'H'}, {34,'J'}, {33,'K'}, {32,'L'}, {18,'Z'}, {17,'X'},
      {16,'C'}, {15,'V'}, {25,'B'}, {24,'N'}, {13,'M'}};
  for (const auto& item : letters) {
    if (item.first == code) {
      const bool upper = g_keyboard_caps != g_keyboard_shift;
      return upper ? item.second : static_cast<std::uint32_t>(item.second - 'A' + 'a');
    }
  }
  return 0;
}

bool activateScreenBackButton() {
  lv_obj_t* screen = lv_screen_active();
  if (screen == nullptr || screen == g_home_screen) return false;
  const std::uint32_t children = lv_obj_get_child_count(screen);
  for (std::uint32_t i = 0; i < children; ++i) {
    lv_obj_t* child = lv_obj_get_child(screen, static_cast<std::int32_t>(i));
    if (lv_obj_has_flag(child, LV_OBJ_FLAG_USER_1)) {
      lv_obj_send_event(child, LV_EVENT_CLICKED, nullptr);
      return true;
    }
  }
  return false;
}

void pollHardwareKeyboard(lv_timer_t*) {
  if (!g_keyboard_ready) return;
  g_keyboard.poll([](const reconclave::KeyEvent& event) {
    const int code = keyboardCode(event);
    if (code == 7) { g_keyboard_shift = event.pressed; return; }
    if (code == 10 && event.pressed) { g_keyboard_caps = !g_keyboard_caps; return; }
    if (!event.pressed) return;
    const std::uint32_t key = keyboardKeyForCode(code);
    if (key == 0) return;
    if (key == LV_KEY_ESC && activateScreenBackButton()) return;
    if (g_keyboard_queue.size() > 48) g_keyboard_queue.pop_front();
    g_keyboard_queue.push_back({key, LV_INDEV_STATE_PRESSED});
    g_keyboard_queue.push_back({key, LV_INDEV_STATE_RELEASED});
  });
}

void readHardwareKeyboard(lv_indev_t*, lv_indev_data_t* data) {
  if (g_keyboard_queue.empty()) {
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }
  const QueuedKey event = g_keyboard_queue.front();
  g_keyboard_queue.pop_front();
  data->key = event.key;
  data->state = event.state;
  data->continue_reading = !g_keyboard_queue.empty();
}

void addDynamicBackButton(lv_obj_t* screen, lv_obj_t** target, lv_obj_t* fallback) {
  using namespace reconclave::ui;
  lv_obj_t* button = lv_button_create(screen);
  lv_obj_set_size(button, 84, kBackButtonHeight);
  lv_obj_align(button, LV_ALIGN_TOP_LEFT, kSafeMargin, kSafeMargin + kStatusBarHeight + 8);
  lv_obj_set_style_bg_color(button, lv_color_hex(kColorPanelLight), 0);
  lv_obj_set_style_border_color(button, lv_color_hex(kColorAccent), 0);
  lv_obj_set_style_border_width(button, 1, 0);
  lv_obj_set_style_radius(button, 8, 0);
  lv_obj_set_style_shadow_width(button, 0, 0);
  lv_obj_add_flag(button, LV_OBJ_FLAG_USER_1);

  struct BackTarget { lv_obj_t** target; lv_obj_t* fallback; };
  auto* back_target = new BackTarget{target, fallback};
  lv_obj_add_event_cb(button, [](lv_event_t* event) {
    auto* back_target = static_cast<BackTarget*>(lv_event_get_user_data(event));
    lv_screen_load(*back_target->target != nullptr ? *back_target->target : back_target->fallback);
  }, LV_EVENT_CLICKED, back_target);
  lv_obj_add_event_cb(button, [](lv_event_t* event) {
    delete static_cast<BackTarget*>(lv_event_get_user_data(event));
  }, LV_EVENT_DELETE, back_target);

  lv_obj_t* label = lv_label_create(button);
  lv_label_set_text(label, LV_SYMBOL_LEFT " back");
  lv_obj_set_style_text_color(label, lv_color_hex(kColorAccent), 0);
  lv_obj_center(label);
}

std::string expansionStatus() {
  const bool modem_uart = pathExists("/dev/ttyS3");
  bool keyboard = false;
  DIR* dir = opendir("/sys/bus/i2c/devices");
  if (dir != nullptr) {
    while (dirent* entry = readdir(dir)) {
      const std::string name = entry->d_name;
      if (name.size() >= 5 && name.compare(name.size() - 5, 5, "-0034") == 0) {
        keyboard = true;
        break;
      }
    }
    closedir(dir);
  }
  std::ostringstream out;
  out << "Modem UART    " << (modem_uart ? "ready; hardware probe pending" : "not exposed") << "\n";
  out << "GNSS          " << (modem_uart ? "interface available" : "unavailable") << "\n";
  out << "Keyboard      " << (keyboard ? "TCA8418 detected" : "awaiting I2C probe");
  return out.str();
}

void refreshStatusBars(lv_timer_t*) {
  char clock[16] = "--:-- ---";
  std::time_t now = std::time(nullptr);
  if (std::tm* local = std::localtime(&now)) std::strftime(clock, sizeof(clock), "%H:%M %Z", local);
  const NetworkState network = networkState();
  const std::string battery = batteryText();
  reconclave::ui::updateStatusBars(clock, battery.c_str(), network.connected);
}

std::string csvField(const std::string& value) {
  bool needs_quotes = value.find(',') != std::string::npos || value.find('"') != std::string::npos;
  if (!needs_quotes) return value;
  std::string escaped = "\"";
  for (char c : value) {
    if (c == '"') escaped += "\"\"";
    else escaped += c;
  }
  escaped += "\"";
  return escaped;
}

void runWifiScan() {
  std::string error;
  bool ok = reconclave::scanWifi("wlan0", g_last_wifi_scan, error);

  std::sort(g_last_wifi_scan.begin(), g_last_wifi_scan.end(), [](const auto& left, const auto& right) {
    return left.rssi_dbm > right.rssi_dbm;
  });

  lv_obj_clean(g_wifi_list);
  for (const auto& ap : g_last_wifi_scan) {
    const char* band = ap.channel > 14 ? "5G" : "2.4G";
    const std::string name = ap.ssid.empty() ? "(hidden network)" : ap.ssid;
    const std::string detail = std::string(band) + "  CH " + std::to_string(ap.channel) + "  |  " +
        std::to_string(ap.rssi_dbm) + " dBm  |  " + std::to_string(ap.quality_percent) + "%  |  " + ap.security;
    lv_obj_t* row = lv_obj_create(g_wifi_list);
    lv_obj_set_size(row, lv_pct(100), 64);
    lv_obj_set_style_bg_color(row, lv_color_hex(reconclave::ui::kColorPanelLight), 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x21485a), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(
        row, lv_color_hex(ap.secured ? reconclave::ui::kColorUnavailable : reconclave::ui::kColorAmber), 0);
    lv_obj_set_style_border_width(row, ap.secured ? 1 : 2, 0);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_layout(row, LV_LAYOUT_NONE);

    lv_obj_t* row_label = lv_label_create(row);
    lv_label_set_text(row_label, name.c_str());
    lv_obj_set_width(row_label, lv_pct(40));
    lv_label_set_long_mode(row_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(row_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(row_label, lv_color_hex(reconclave::ui::kColorForeground), 0);
    lv_obj_align(row_label, LV_ALIGN_TOP_LEFT, 16, 10);

    lv_obj_t* signal = lv_bar_create(row);
    lv_obj_set_size(signal, 280, 5);
    lv_obj_align(signal, LV_ALIGN_BOTTOM_LEFT, 16, -9);
    lv_bar_set_range(signal, 0, 100);
    lv_bar_set_value(signal, std::clamp(ap.quality_percent, 0, 100), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(signal, lv_color_hex(0x21414d), LV_PART_MAIN);
    const std::uint32_t signal_color = ap.quality_percent >= 70 ? reconclave::ui::kColorAccent
        : (ap.quality_percent >= 40 ? reconclave::ui::kColorAmber : reconclave::ui::kColorUnavailable);
    lv_obj_set_style_bg_color(signal, lv_color_hex(signal_color), LV_PART_INDICATOR);
    lv_obj_set_style_radius(signal, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(signal, 3, LV_PART_INDICATOR);

    lv_obj_t* detail_label = lv_label_create(row);
    lv_label_set_text(detail_label, detail.c_str());
    lv_obj_set_width(detail_label, lv_pct(55));
    lv_label_set_long_mode(detail_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(detail_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(detail_label,
        lv_color_hex(ap.secured ? reconclave::ui::kColorSecondary : reconclave::ui::kColorAmber), 0);
    lv_obj_align(detail_label, LV_ALIGN_RIGHT_MID, -14, 0);
  }

  std::string status;
  if (ok) {
    const auto survey = reconclave::analyseWifi(g_last_wifi_scan);
    status = std::to_string(survey.total) + " APs  |  " + std::to_string(survey.two_ghz) + " x 2.4G  " +
             std::to_string(survey.five_ghz) + " x 5G  |  open " + std::to_string(survey.open) +
             "  hidden " + std::to_string(survey.hidden) + "  |  best ch " +
             std::to_string(survey.best_24_channel);
  } else {
    status = "Scan failed: " + error;
  }
  lv_label_set_text(g_wifi_status, status.c_str());
}

void exportWifiEvidence() {
  if (g_last_wifi_scan.empty()) {
    lv_label_set_text(g_evidence_status, "Nothing to export - run a Wi-Fi scan first");
    return;
  }

  // Same directory convention as reconclave_k230's deployment
  // (/root/reconclave/), on the rootfs itself - this board has no separate
  // removable microSD slot (confirmed: only one mmc host has a card;
  // see devices/k230/README.md), so "evidence storage" here means a
  // directory on the same persistent storage as everything else, not a
  // swappable card like Cardputer's evidence workflow.
  if ((mkdir("/root/reconclave", 0755) != 0 && errno != EEXIST) ||
      (mkdir(kEvidenceDir, 0755) != 0 && errno != EEXIST)) {
    lv_label_set_text(g_evidence_status, "Could not create evidence directory");
    return;
  }

  std::time_t now = std::time(nullptr);
  char timestamp[32];
  std::strftime(timestamp, sizeof(timestamp), "%Y%m%d-%H%M%S", std::localtime(&now));
  std::string path = std::string(kEvidenceDir) + "/wifi-" + timestamp + ".csv";

  std::ofstream file(path);
  if (!file) {
    lv_label_set_text(g_evidence_status, "Write failed");
    return;
  }
  file << "project_id,engagement_id,operator_id,ssid,bssid,channel,frequency_mhz,band,rssi_dbm,quality_percent,security,observer_node,latitude,longitude,location_valid\n";
  const auto& fix = g_gnss.fix();
  for (const auto& ap : g_last_wifi_scan) {
    file << csvField(g_project_id) << "," << csvField(g_engagement_id) << "," << csvField(g_operator_id)
         << "," << csvField(ap.ssid) << "," << ap.bssid << "," << ap.channel << "," << ap.frequency_mhz << ","
         << (ap.channel > 14 ? "5GHz" : "2.4GHz") << "," << ap.rssi_dbm << "," << ap.quality_percent << ","
         << ap.security << "," << kDeviceId << ",";
    if (fix.valid) file << std::fixed << std::setprecision(7) << fix.latitude << "," << fix.longitude << ",true\n";
    else file << ",,false\n";
  }
  file.close();

  std::string status = "Saved " + std::to_string(g_last_wifi_scan.size()) + " networks to " + path;
  lv_label_set_text(g_evidence_status, status.c_str());
  refreshEvidenceSummary();
}

// Scales `image` (already pointed at "A:"+path via lv_image_set_src) so a
// fixed kCaptureWidth x kCaptureHeight source fits inside box_w x box_h
// without distortion - LVGL doesn't auto-fit file-backed images to their
// parent, and the capture resolution is fixed (see camera_capture.cpp), so
// this is cheaper than querying the decoder for dimensions first.
void setImageContain(lv_obj_t* image, const std::string& path, lv_coord_t box_w, lv_coord_t box_h) {
  const std::string src = "A:" + path;
  // Preview files are replaced in place. Drop any decoded copy before
  // assigning the source so LVGL cannot display the previous frame from
  // its image cache.
  lv_image_cache_drop(nullptr);
  lv_image_set_src(image, src.c_str());
  const int scale_w = static_cast<int>(256.0 * box_w / kCaptureWidth);
  const int scale_h = static_cast<int>(256.0 * box_h / kCaptureHeight);
  lv_image_set_scale(image, std::max(16, std::min(scale_w, scale_h)));
}

void showLastPhoto(const std::string& path, lv_obj_t* thumbnail_target) {
  g_last_photo_path = path;
  if (thumbnail_target != nullptr) {
    // Size against the image's *parent* (the fixed-size panel it sits in),
    // not the lv_image object itself - an lv_image with no source loaded
    // yet reports a 0x0 (or content-driven, effectively meaningless) size,
    // which made setImageContain() compute a near-zero scale and rendered
    // nothing on the first call (see openPhotoViewer(), which already did
    // this correctly).
    lv_obj_t* box = lv_obj_get_parent(thumbnail_target);
    setImageContain(thumbnail_target, path, lv_obj_get_width(box), lv_obj_get_height(box));
  }
}

void openPhotoViewer() {
  if (g_photo_viewer_screen == nullptr || g_last_photo_path.empty()) return;
  // Scale to the actual black frame the image sits in (its parent), not
  // the whole screen's content area - the caption row below it needs its
  // own space, see buildPhotoViewerScreen().
  lv_obj_t* frame = lv_obj_get_parent(g_photo_viewer_image);
  setImageContain(g_photo_viewer_image, g_last_photo_path, lv_obj_get_width(frame),
                  lv_obj_get_height(frame));
  lv_obj_center(g_photo_viewer_image);
  lv_label_set_text(g_photo_viewer_caption, g_last_photo_path.c_str());
  lv_screen_load(g_photo_viewer_screen);
}

// Live preview runs on its own thread rather than blocking the LVGL/UI
// thread for each ~1s camera capture: an earlier version called
// captureStill() directly from a ~1.2s LVGL timer, which meant the single
// UI thread spent the large majority of every cycle blocked inside a
// subprocess wait - lv_timer_handler() (which also dispatches touch input)
// simply wasn't running most of the time, so the whole app felt frozen,
// not just choppy. The worker below does the actual capture I/O; the
// LVGL-side timer (checkPreviewWorker, still on the UI thread) only ever
// does a cheap atomic read and, when a new frame is ready, the same
// non-blocking lv_image_set_src() work any other screen refresh does.
//
// The worker never touches an lv_obj_t directly (LVGL isn't thread-safe
// for concurrent object access) - it only publishes a path and bumps a
// generation counter under g_preview_mutex; only the UI-thread timer
// applies that to the actual widgets.
std::atomic<bool> g_preview_worker_running{false};
std::atomic<std::uint64_t> g_preview_generation{0};
std::mutex g_preview_mutex;
bool g_preview_last_ok = false;
std::string g_preview_last_error;
std::string g_preview_last_path;
std::thread g_preview_thread;
std::uint64_t g_preview_applied_generation = 0;  // UI-thread only, no lock needed.

void previewWorkerLoop() {
  unsigned int frame_slot = 0;
  while (g_preview_worker_running.load(std::memory_order_relaxed)) {
    const char* preview_path = kPreviewPaths[frame_slot++ % 2];
    const auto capture = reconclave::captureStill(kCameraDevice, preview_path, 3000, true);
    {
      std::lock_guard<std::mutex> lock(g_preview_mutex);
      g_preview_last_ok = capture.ok;
      g_preview_last_error = capture.error;
      g_preview_last_path = capture.ok ? preview_path : "";
    }
    g_preview_generation.fetch_add(1, std::memory_order_release);
    // A short gap between captures rather than looping flat-out - the
    // encode+write should be fully flushed before the next open, and this
    // keeps ffmpeg from being re-invoked literally back-to-back.
    for (int waited = 0; waited < 3 && g_preview_worker_running.load(std::memory_order_relaxed);
        ++waited) {
      usleep(100000);
    }
  }
}

void startPreviewWorker() {
  if (g_preview_worker_running.exchange(true)) return;  // already running
  g_preview_thread = std::thread(previewWorkerLoop);
}

void stopPreviewWorker() {
  if (!g_preview_worker_running.exchange(false)) return;  // wasn't running
  if (g_preview_thread.joinable()) g_preview_thread.join();
}

void updatePreviewButton(bool running) {
  if (g_vision_preview_button == nullptr || g_vision_preview_button_label == nullptr) return;
  lv_label_set_text(g_vision_preview_button_label,
                    running ? LV_SYMBOL_STOP " Stop preview" : LV_SYMBOL_PLAY " Start preview");
  lv_obj_set_style_bg_color(g_vision_preview_button,
                            lv_color_hex(running ? 0x71333a : reconclave::ui::kColorPanelLight), 0);
}

void togglePreview() {
  if (g_preview_worker_running.load(std::memory_order_relaxed)) {
    stopPreviewWorker();
    if (g_vision_preview_timer != nullptr) lv_timer_pause(g_vision_preview_timer);
    updatePreviewButton(false);
    if (g_vision_status != nullptr) lv_label_set_text(g_vision_status, "PREVIEW PAUSED");
    return;
  }
  g_preview_applied_generation = g_preview_generation.load(std::memory_order_acquire);
  if (g_vision_preview_timer != nullptr) lv_timer_resume(g_vision_preview_timer);
  startPreviewWorker();
  updatePreviewButton(true);
  if (g_vision_status != nullptr) {
    lv_label_set_text(g_vision_status, "STARTING CAMERA  //  The first frame may take a moment.");
  }
}

// Cheap, non-blocking: runs every ~150ms on the UI thread while VISION is
// loaded (see buildVisionScreen), but does real work only on the ~once-a-
// second tick where the worker has actually produced a new frame.
void checkPreviewWorker(lv_timer_t*) {
  const auto generation = g_preview_generation.load(std::memory_order_acquire);
  if (generation == g_preview_applied_generation) return;  // nothing new yet
  g_preview_applied_generation = generation;

  bool ok = false;
  std::string error;
  std::string path;
  {
    std::lock_guard<std::mutex> lock(g_preview_mutex);
    ok = g_preview_last_ok;
    error = g_preview_last_error;
    path = g_preview_last_path;
  }
  if (!ok) {
    if (g_vision_status != nullptr) {
      lv_label_set_text(g_vision_status, ("Capture failed: " + error).c_str());
    }
    return;
  }
  showLastPhoto(path, g_vision_preview);
  if (g_vision_preview_hint != nullptr) lv_obj_add_flag(g_vision_preview_hint, LV_OBJ_FLAG_HIDDEN);
  if (g_vision_status != nullptr) {
    lv_label_set_text(g_vision_status, "LIVE  //  Tap the viewfinder to inspect the current frame.");
  }
}

// Capture photo: a single, deliberate, synchronous shot straight into the
// evidence store. Unlike the live preview this is a one-off user action
// (like a real camera's shutter button), so a ~1s block here is expected
// shutter latency, not a background loop fighting the UI thread for time.
void captureEvidencePhoto() {
  if (g_vision_status == nullptr) return;
  stopPreviewWorker();  // don't contend with the worker for the camera device
  if (g_vision_preview_timer != nullptr) lv_timer_pause(g_vision_preview_timer);
  updatePreviewButton(false);
  if (!pathExists(kCameraDevice)) {
    lv_label_set_text(g_vision_status, "Camera device not present");
    return;
  }
  if ((mkdir("/root/reconclave", 0755) != 0 && errno != EEXIST) ||
      (mkdir(kEvidenceDir, 0755) != 0 && errno != EEXIST)) {
    lv_label_set_text(g_vision_status, "Could not create evidence directory");
    return;
  }
  std::time_t now = std::time(nullptr);
  char timestamp[32];
  std::strftime(timestamp, sizeof(timestamp), "%Y%m%d-%H%M%S", std::localtime(&now));
  const std::string path = std::string(kEvidenceDir) + "/photo-" + timestamp + ".jpg";

  lv_label_set_text(g_vision_status, "Capturing...");
  lv_refr_now(nullptr);
  const auto capture = reconclave::captureStill(kCameraDevice, path, 6000);
  if (!capture.ok) {
    lv_label_set_text(g_vision_status, ("Capture failed: " + capture.error).c_str());
    return;
  }
  showLastPhoto(path, g_vision_preview);
  if (g_vision_preview_hint != nullptr) lv_obj_add_flag(g_vision_preview_hint, LV_OBJ_FLAG_HIDDEN);
  const std::string status = "Saved " + formatBytes(capture.size_bytes) + " to " + path;
  lv_label_set_text(g_vision_status, status.c_str());
  refreshEvidenceSummary();
}

// Runs argv[0] (a bare command name, resolved via PATH - execvp, no
// shell, matching every other subprocess call in this app) with a
// bounded wait; SIGKILLs and reports failure rather than blocking
// forever if it hangs. Captures combined stdout+stderr for error
// messages (reconclave-wifi's own diagnostics, e.g. "wrong password").
struct SimpleCommandResult { bool ok = false; std::string output; };

SimpleCommandResult runSimpleCommand(const std::vector<std::string>& argv, int timeout_ms,
                                     const std::string& stdin_data = {}) {
  SimpleCommandResult result;
  int output_pipe[2];
  int input_pipe[2];
  if (pipe(output_pipe) != 0 || pipe(input_pipe) != 0) {
    result.output = "pipe() failed";
    return result;
  }
  const pid_t pid = fork();
  if (pid < 0) {
    result.output = "fork() failed";
    close(output_pipe[0]); close(output_pipe[1]);
    close(input_pipe[0]); close(input_pipe[1]);
    return result;
  }
  if (pid == 0) {
    dup2(input_pipe[0], STDIN_FILENO);
    dup2(output_pipe[1], STDOUT_FILENO);
    dup2(output_pipe[1], STDERR_FILENO);
    close(input_pipe[0]); close(input_pipe[1]);
    close(output_pipe[0]); close(output_pipe[1]);
    std::vector<char*> exec_argv;
    exec_argv.reserve(argv.size() + 1);
    for (const auto& arg : argv) exec_argv.push_back(const_cast<char*>(arg.c_str()));
    exec_argv.push_back(nullptr);
    execvp(exec_argv[0], exec_argv.data());
    _exit(127);
  }
  close(input_pipe[0]);
  close(output_pipe[1]);
  if (!stdin_data.empty()) {
    const std::string wire = stdin_data + "\n";
    std::size_t sent = 0;
    while (sent < wire.size()) {
      const ssize_t count = write(input_pipe[1], wire.data() + sent, wire.size() - sent);
      if (count <= 0) break;
      sent += static_cast<std::size_t>(count);
    }
  }
  close(input_pipe[1]);
  const int original_flags = fcntl(output_pipe[0], F_GETFL, 0);
  fcntl(output_pipe[0], F_SETFL, original_flags | O_NONBLOCK);
  char buffer[512];
  int status = 0;
  int elapsed_ms = 0;
  bool exited = false;
  while (elapsed_ms < timeout_ms) {
    ssize_t n;
    while ((n = read(output_pipe[0], buffer, sizeof(buffer))) > 0) result.output.append(buffer, n);
    const pid_t outcome = waitpid(pid, &status, WNOHANG);
    if (outcome == pid) { exited = true; break; }
    if (outcome < 0) break;
    usleep(100000);
    elapsed_ms += 100;
  }
  if (!exited) {
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    result.output += "\n(timed out)";
  } else {
    ssize_t n;
    while ((n = read(output_pipe[0], buffer, sizeof(buffer))) > 0) result.output.append(buffer, n);
    result.ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
  }
  close(output_pipe[0]);
  return result;
}

struct WifiLinkStatus { bool associated = false; std::string ssid; std::string ip; };

WifiLinkStatus currentWifiLinkStatus() {
  WifiLinkStatus status;
  const auto iwconfig = runSimpleCommand({"iwconfig", "wlan0"}, 3000);
  const auto essid_pos = iwconfig.output.find("ESSID:\"");
  if (essid_pos != std::string::npos) {
    const auto start = essid_pos + std::strlen("ESSID:\"");
    const auto end = iwconfig.output.find('"', start);
    if (end != std::string::npos) {
      status.ssid = iwconfig.output.substr(start, end - start);
      status.associated = !status.ssid.empty();
    }
  }
  ifaddrs* addrs = nullptr;
  if (getifaddrs(&addrs) == 0) {
    for (ifaddrs* it = addrs; it != nullptr; it = it->ifa_next) {
      if (it->ifa_name == nullptr || std::string(it->ifa_name) != "wlan0" || it->ifa_addr == nullptr ||
          it->ifa_addr->sa_family != AF_INET) {
        continue;
      }
      char text[INET_ADDRSTRLEN]{};
      const auto* in = reinterpret_cast<sockaddr_in*>(it->ifa_addr);
      if (inet_ntop(AF_INET, &in->sin_addr, text, sizeof(text)) != nullptr) status.ip = text;
      break;
    }
    freeifaddrs(addrs);
  }
  return status;
}

// Shared by connect and disconnect - only one Wi-Fi op runs at a time
// (g_wifi_op_running), and the actual command work happens entirely on
// the worker thread; nothing here touches an lv_obj_t (matches the
// camera preview worker's own rule - LVGL isn't thread-safe for
// concurrent object access from a background thread).
void runWifiOpAsync(std::function<void()> work) {
  if (g_wifi_op_running.exchange(true)) return;
  if (g_wifi_op_thread.joinable()) g_wifi_op_thread.join();
  g_wifi_op_thread = std::thread([work]() {
    work();
    g_wifi_op_generation.fetch_add(1, std::memory_order_release);
    g_wifi_op_running.store(false, std::memory_order_release);
  });
}

void startWifiConnect(const std::string& ssid, const std::string& password) {
  runWifiOpAsync([ssid, password]() {
    // Feed the PSK over stdin so it never appears in ps/proc command lines.
    const auto connect_result = runSimpleCommand({"reconclave-wifi", ssid}, 25000, password);
    bool associated_with_ip = false;
    std::string ip;
    if (connect_result.ok) {
      // The script itself only reports "profile installed", not a
      // completed DHCP lease (it forks udhcpc to the background on
      // failure and returns immediately either way) - poll briefly for
      // an actual association + address rather than trusting exit 0.
      for (int attempt = 0; attempt < 16; ++attempt) {
        const auto link = currentWifiLinkStatus();
        if (link.associated && link.ssid == ssid && !link.ip.empty()) {
          associated_with_ip = true;
          ip = link.ip;
          break;
        }
        usleep(500000);
      }
    }
    std::lock_guard<std::mutex> lock(g_wifi_op_mutex);
    g_wifi_op_ok = associated_with_ip;
    if (associated_with_ip) {
      g_wifi_op_message = "Connected to " + ssid + "  |  " + ip;
    } else {
      std::string detail = connect_result.output;
      while (!detail.empty() && (detail.back() == '\n' || detail.back() == '\r')) detail.pop_back();
      g_wifi_op_message = "Could not connect to " + ssid +
          (detail.empty() ? "" : (" (" + detail.substr(detail.find_last_of('\n') + 1) + ")"));
    }
  });
}

void startWifiDisconnect() {
  runWifiOpAsync([]() {
    // ifdown's own post-down hook stops wpa_supplicant cleanly - a raw
    // `killall wpa_supplicant` while it's still mid-association/retry is
    // what actually caused a device hang once (see README), not Wi-Fi
    // interface cycling in general.
    runSimpleCommand({"ifdown", "wlan0"}, 10000);
    std::lock_guard<std::mutex> lock(g_wifi_op_mutex);
    g_wifi_op_ok = false;
    g_wifi_op_message = "Disconnected";
  });
}

void createEvidenceManifest() {
  reconclave::EvidenceStore evidence(kEvidenceDir, kDeviceId);
  reconclave::json::Value manifest;
  std::string error;
  if (!evidence.createManifest(reconclave::loadEvidenceContext(kSessionPath), kFirmware,
                               manifest, error)) {
    const std::string status = "Manifest blocked: " + error;
    lv_label_set_text(g_evidence_status, status.c_str());
    return;
  }
  const auto* hash = manifest.find("manifest_hash");
  const std::string short_hash = hash ? hash->asString().substr(0, 12) : "unknown";
  const std::string status = "Manifest verified and saved  |  " + short_hash;
  lv_label_set_text(g_evidence_status, status.c_str());
  refreshEvidenceSummary();
}

void startUiScreenshot(lv_obj_t* status) {
  if (g_screenshot_pid > 0) {
    if (status != nullptr) lv_label_set_text(status, "Screenshot capture already active");
    return;
  }
  const pid_t child = fork();
  if (child < 0) {
    if (status != nullptr) lv_label_set_text(status, "Screenshot failed to start");
    return;
  }
  if (child == 0) {
    execlp("reconclave-screenshot", "reconclave-screenshot", static_cast<char*>(nullptr));
    _exit(127);
  }
  g_screenshot_pid = child;
  g_screenshot_action_status = status;
  if (status != nullptr) lv_label_set_text(status, "Saving UI screenshot to evidence...");
}

void refreshEvidenceSummary() {
  if (g_evidence_summary == nullptr) return;
  DIR* dir = opendir(kEvidenceDir);
  int files = 0;
  uint64_t bytes = 0;
  std::string newest;
  std::vector<std::string> recent;
  if (dir != nullptr) {
    while (dirent* entry = readdir(dir)) {
      if (entry->d_name[0] == '.') continue;
      std::string path = std::string(kEvidenceDir) + "/" + entry->d_name;
      struct stat info {};
      if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) continue;
      ++files;
      bytes += static_cast<uint64_t>(info.st_size);
      recent.push_back(entry->d_name);
      if (newest.empty() || newest < entry->d_name) newest = entry->d_name;
    }
    closedir(dir);
  }
  std::sort(recent.begin(), recent.end(), std::greater<std::string>());
  std::string text = "EVIDENCE STORE\n\n" + std::to_string(files) + " artifacts  //  " + formatBytes(bytes);
  const reconclave::EvidenceStore evidence(kEvidenceDir, kDeviceId);
  const auto verification = evidence.verify();
  text += "\nTimeline: ";
  if (verification.valid) {
    text += "CHAIN VERIFIED  //  " + std::to_string(verification.records) + " records";
  } else {
    text += "FAILED at record " + std::to_string(verification.first_invalid_record);
  }
  text += "\n\nRECENT ARTIFACTS";
  if (recent.empty()) {
    text += "\nNo captures saved yet";
  } else {
    const std::size_t shown = std::min<std::size_t>(recent.size(), 7);
    for (std::size_t i = 0; i < shown; ++i) text += "\n" LV_SYMBOL_FILE "  " + recent[i];
  }
  lv_label_set_text(g_evidence_summary, text.c_str());
}

void refreshNetworkDetails() {
  if (g_network_details == nullptr) return;
  const NetworkState state = networkState();
  const std::string target = attachedIpv4Network();
  const std::string speed = readFirstLine("/sys/class/net/" + state.iface + "/speed");
  const int neighbours = arpNeighbourCount();
  if (g_network_summary != nullptr) {
    std::ostringstream summary;
    summary << (state.connected ? LV_SYMBOL_OK "  LINK ONLINE" : LV_SYMBOL_CLOSE "  LINK OFFLINE")
            << "\n\nACTIVE INTERFACE\n" << state.iface
            << "\n\nASSESSMENT SCOPE\n" << (target.empty() ? "No attached IPv4 network" : target)
            << "\n\nPASSIVE NEIGHBOURS\n" << neighbours;
    lv_label_set_text(g_network_summary, summary.str().c_str());
  }
  std::ostringstream out;
  const std::string rx = readFirstLine("/sys/class/net/" + state.iface + "/statistics/rx_bytes");
  const std::string tx = readFirstLine("/sys/class/net/" + state.iface + "/statistics/tx_bytes");
  out << "INTERFACE TELEMETRY\n\n"
      << "IPv4 ADDRESS\n" << state.address << "\n\n"
      << "MAC ADDRESS\n" << readFirstLine("/sys/class/net/" + state.iface + "/address") << "\n\n"
      << "LINK STATE\n" << readFirstLine("/sys/class/net/" + state.iface + "/operstate");
  if (!speed.empty()) out << "  //  " << speed << " Mbps";
  out << "\n\nDEFAULT ROUTE\n" << state.gateway << "\n\n"
      << "TRAFFIC RX / TX\n"
      << formatBytes(std::strtoull(rx.c_str(), nullptr, 10)) << "  /  "
      << formatBytes(std::strtoull(tx.c_str(), nullptr, 10));
  const std::string wifi_address = interfaceIpv4Address("wlan0");
  if (state.iface != "wlan0" && pathExists("/sys/class/net/wlan0")) {
    out << "\n\nSECONDARY LINK\nwlan0  //  "
        << (wifi_address.empty() ? readFirstLine("/sys/class/net/wlan0/operstate") : wifi_address);
  }
  lv_label_set_text(g_network_details, out.str().c_str());
}

std::string nodeStatusValue(const std::string& status, const std::string& key) {
  std::istringstream lines(status);
  std::string line;
  while (std::getline(lines, line)) {
    if (line.rfind(key, 0) != 0) continue;
    const auto start = line.find_first_not_of(" \t", key.size());
    return start == std::string::npos ? std::string() : line.substr(start);
  }
  return {};
}

void refreshNodeDetails() {
  const std::string status = readTextFile("/run/reconclave/node-status");
  if (status.empty()) {
    lv_label_set_text(g_node_details, "NODE IDENTITY\n\n" LV_SYMBOL_CLOSE "  SERVICE OFFLINE");
    if (g_node_trust != nullptr) lv_label_set_text(g_node_trust, "TRUST & ENDPOINT\n\nUnavailable");
    if (g_node_job != nullptr) lv_label_set_text(g_node_job, "CURRENT JOB\n\nNo runtime status");
    return;
  }
  const auto value = [&](const char* key, const char* fallback = "--") {
    const std::string found = nodeStatusValue(status, key);
    return found.empty() ? std::string(fallback) : found;
  };
  std::ostringstream identity;
  identity << "NODE IDENTITY\n\nROLE\n" << value("ROLE")
           << "\n\nDEVICE\n" << value("DEVICE")
           << "\n\nCAPABILITIES\n" << value("CAPABILITIES") << " enabled";
  lv_label_set_text(g_node_details, identity.str().c_str());
  if (g_node_trust != nullptr) {
    const std::string trust = value("TRUST");
    std::ostringstream remote;
    remote << "TRUST & ENDPOINT\n\nEXECUTION TRUST\n" << trust
           << "\n\nREMOTE CONTROL\n"
           << (trust == "provisioned" ? LV_SYMBOL_OK "  SIGNED JOBS ENABLED"
                                      : LV_SYMBOL_WARNING "  BLOCKED UNTIL PROVISIONED")
           << "\n\nLISTEN ENDPOINT\n" << value("ADDRESS");
    lv_label_set_text(g_node_trust, remote.str().c_str());
  }
  if (g_node_job != nullptr) {
    std::ostringstream job;
    job << "CURRENT JOB\n\nJOB\n" << value("JOB", "none")
        << "\n\nSTATE\n" << value("JOB STATUS", "idle")
        << "\n\nPROGRESS\n" << value("PROGRESS", "0/0")
        << "\n\nHOSTS FOUND\n" << value("HOSTS FOUND", "0")
        << "  //  " << value("KNOWN HOSTS", "0") << " known";
    lv_label_set_text(g_node_job, job.str().c_str());
  }
}

void refreshLiveDetails(lv_timer_t*) {
  const std::string ui_command = readFirstLine(kUiCommandPath);
  if (!ui_command.empty()) {
    unlink(kUiCommandPath);
    constexpr char kPrefix[] = "screen ";
    if (ui_command.rfind(kPrefix, 0) == 0) {
      const std::string requested = ui_command.substr(sizeof(kPrefix) - 1);
      const auto target = std::find_if(g_ui_screens.begin(), g_ui_screens.end(),
          [&](const auto& entry) { return entry.first == requested; });
      if (target != g_ui_screens.end() && target->second != nullptr) lv_screen_load(target->second);
    }
  }
  g_gnss.poll();
  writeGnssRuntimeStatus();
  refreshBatteryState();
  writeBatteryRuntimeStatus();
  refreshStatusBars(nullptr);
  refreshNetworkDetails();
  if (g_home_context != nullptr) {
    const NetworkState network = networkState();
    std::ostringstream context;
    context << "PROJECT\n" << g_project_id << "\n\nENGAGEMENT\n" << g_engagement_id
            << "\n\nOPERATOR\n" << g_operator_id << "\n\nTARGET\n"
            << (attachedIpv4Network().empty() ? "OFFLINE" : attachedIpv4Network());
    lv_label_set_text(g_home_context, context.str().c_str());
    std::ostringstream readiness;
    readiness << (network.connected ? LV_SYMBOL_OK " LINK READY" : LV_SYMBOL_CLOSE " OFFLINE")
              << "   " << arpNeighbourCount() << " NEIGHBOURS\n"
              << (g_keyboard_ready ? LV_SYMBOL_KEYBOARD " KEYBOARD" : "TOUCH MODE")
              << "   " << batteryText();
    lv_label_set_text(g_home_readiness, readiness.str().c_str());
  }
  if (g_capture_pid > 0) {
    int status = 0;
    const pid_t done = waitpid(g_capture_pid, &status, WNOHANG);
    if (done == g_capture_pid) {
      g_capture_pid = -1;
      if (g_capture_stop_button != nullptr) lv_obj_add_state(g_capture_stop_button, LV_STATE_DISABLED);
      std::string output = readTextFile("/tmp/reconclave-capture.log");
      output.erase(std::remove(output.begin(), output.end(), '\n'), output.end());
      if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        lv_label_set_text(g_capture_status,
            (LV_SYMBOL_OK " Capture saved\n" + output).c_str());
        refreshEvidenceSummary();
      } else {
        lv_label_set_text(g_capture_status,
            (LV_SYMBOL_WARNING " Capture failed\n" + output).c_str());
      }
    }
  }
  if (g_nmap_pid > 0) {
    int status = 0;
    if (waitpid(g_nmap_pid, &status, WNOHANG) == g_nmap_pid) {
      g_nmap_pid = -1;
      std::string output = readTextFile("/tmp/reconclave-nmap.log");
      if (output.size() > 12000) output = output.substr(output.size() - 12000);
      lv_label_set_text(g_nmap_output, output.c_str());
      refreshEvidenceSummary();
    }
  }
  if (g_screenshot_pid > 0) {
    int status = 0;
    if (waitpid(g_screenshot_pid, &status, WNOHANG) == g_screenshot_pid) {
      g_screenshot_pid = -1;
      const bool saved = WIFEXITED(status) && WEXITSTATUS(status) == 0;
      if (g_screenshot_action_status != nullptr) {
        lv_label_set_text(g_screenshot_action_status,
                          saved ? LV_SYMBOL_OK " Screenshot saved to evidence"
                                : LV_SYMBOL_WARNING " Screenshot capture failed");
      }
      if (saved) refreshEvidenceSummary();
      g_screenshot_action_status = nullptr;
    }
  }
  if (g_device_details != nullptr) {
    refreshDeviceDetails();
  }
  if (g_recon_details != nullptr) {
    const std::string detail = reconDashboardText();
    lv_label_set_text(g_recon_details, detail.c_str());
  }
  if (g_gnss_details != nullptr) {
    const auto& fix = g_gnss.fix();
    std::ostringstream details;
    if (!kGnssAvailable) {
      details << LV_SYMBOL_WARNING "  GNSS MODULE UNAVAILABLE\n"
              << "nRF9151  //  KEYBOARD EXPANSION UART DETECTED\n\n"
              << "POSITION        --\nSATELLITES      --\nFIX AGE         --\n\n"
              << "The installed modem firmware does not provide the required GNSS command set.\n\n"
              << "EVIDENCE GEO-TAGGING\nDisabled  //  captures remain explicitly marked location_valid=false";
    } else if (fix.valid) {
      details << LV_SYMBOL_GPS "  " << g_gnss.status() << "\n\n";
      details << std::fixed << std::setprecision(7) << "LATITUDE     " << fix.latitude
              << "\nLONGITUDE    " << fix.longitude << std::setprecision(1)
              << "\nALTITUDE     " << fix.altitude_m << " m"
              << "\nSATELLITES   " << fix.satellites
              << "\nSPEED        " << fix.speed_knots << " kn"
              << "\nUTC          " << fix.utc << "\n\nEvidence geotagging active";
    } else {
      details << (g_gnss.running() ? LV_SYMBOL_GPS "  " : LV_SYMBOL_WARNING "  ")
              << g_gnss.status() << "\n\n";
      details << "No valid position yet\n\nMove outdoors with a clear sky view. A cold fix may take several minutes."
              << "\n\nWi-Fi evidence remains explicitly marked location_valid=false until a fix is acquired.";
    }
    lv_label_set_text(g_gnss_details, details.str().c_str());
  }
  if (g_node_details != nullptr) {
    refreshNodeDetails();
  }
  if (g_settings_trust_status != nullptr) {
    lv_label_set_text(g_settings_trust_status, trustStatusLine().c_str());
  }
  if (g_battery_details != nullptr) {
    lv_label_set_text(g_battery_details, batteryDetailsText().c_str());
  }
  {
    // Picks up a completed connect/disconnect from the Wi-Fi worker
    // thread - cheap when nothing changed (one atomic compare), so this
    // runs unconditionally here rather than needing its own gated timer.
    const auto generation = g_wifi_op_generation.load(std::memory_order_acquire);
    if (generation != g_wifi_op_applied_generation) {
      g_wifi_op_applied_generation = generation;
      std::string message;
      {
        std::lock_guard<std::mutex> lock(g_wifi_op_mutex);
        message = g_wifi_op_message;
      }
      if (g_wifisetup_status != nullptr) lv_label_set_text(g_wifisetup_status, message.c_str());
      if (g_wifi_password_status != nullptr) lv_label_set_text(g_wifi_password_status, message.c_str());
    }
  }
  refreshReconHostList();
  refreshFindingsList();
  if (!g_host_detail_address.empty()) populateHostDetail(g_host_detail_address);
}

std::string formatEpochMs(std::uint64_t ms) {
  if (ms == 0) return "unknown";
  const std::time_t seconds = static_cast<std::time_t>(ms / 1000);
  char buffer[32];
  if (std::tm* local = std::localtime(&seconds)) {
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", local);
    return buffer;
  }
  return "unknown";
}

// Shared row style for the tappable host and findings lists - same visual
// language as the Wi-Fi scan list rows in runWifiScan().
lv_obj_t* makeListRow(lv_obj_t* list, std::uint32_t border_color, lv_coord_t height) {
  using namespace reconclave::ui;
  lv_obj_t* row = lv_obj_create(list);
  lv_obj_set_size(row, lv_pct(100), height);
  lv_obj_set_style_bg_color(row, lv_color_hex(kColorPanelLight), 0);
  lv_obj_set_style_bg_color(row, lv_color_hex(0x21485a), LV_STATE_PRESSED);
  lv_obj_set_style_border_color(row, lv_color_hex(border_color), 0);
  lv_obj_set_style_border_width(row, 1, 0);
  lv_obj_set_style_radius(row, 8, 0);
  lv_obj_set_style_pad_all(row, 10, 0);
  lv_obj_set_style_shadow_width(row, 0, 0);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
  return row;
}

// Wires `row` to open the Host Detail screen for `address` when tapped,
// remembering `return_screen` so Host Detail's own back button comes back
// to whichever list (RECON or Findings) it was opened from. The context is
// heap-allocated once per row and freed on LV_EVENT_DELETE - list rows are
// recreated wholesale (lv_obj_clean) on every refresh, so this ties its
// lifetime to the row it belongs to rather than leaking one small
// allocation per refresh tick.
void attachHostAddress(lv_obj_t* row, const std::string& address, lv_obj_t* return_screen) {
  struct RowContext { std::string address; lv_obj_t* return_screen; };
  auto* stored = new RowContext{address, return_screen};
  lv_obj_add_event_cb(row, [](lv_event_t* event) {
    auto* context = static_cast<RowContext*>(lv_event_get_user_data(event));
    if (context != nullptr) showHostDetail(context->address, context->return_screen);
  }, LV_EVENT_CLICKED, stored);
  lv_obj_add_event_cb(row, [](lv_event_t* event) {
    delete static_cast<RowContext*>(lv_event_get_user_data(event));
  }, LV_EVENT_DELETE, stored);
}

void refreshReconHostList() {
  using namespace reconclave::ui;
  if (g_recon_host_list == nullptr) return;
  reconclave::HostKnowledge knowledge(kHostKnowledgePath);
  std::string error;
  knowledge.load(error);
  const auto hosts = knowledge.snapshot();
  lv_obj_clean(g_recon_host_list);
  if (hosts.empty()) {
    lv_obj_t* empty = lv_label_create(g_recon_host_list);
    lv_label_set_text(empty, "No hosts accumulated yet. Survey the attached LAN or allow passive observation.");
    lv_obj_set_width(empty, lv_pct(100));
    lv_label_set_long_mode(empty, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_color(empty, lv_color_hex(kColorSecondary), 0);
    lv_obj_set_style_text_font(empty, &lv_font_montserrat_14, 0);
    return;
  }
  for (const auto& host : hosts) {
    const bool has_review = std::any_of(host.services.begin(), host.services.end(),
        [](const auto& service) { return !service.attention.empty(); });
    lv_obj_t* row = makeListRow(g_recon_host_list, has_review ? kColorAmber : kColorAccent, 58);

    lv_obj_t* address_label = lv_label_create(row);
    lv_label_set_text(address_label, host.address.c_str());
    lv_obj_set_width(address_label, lv_pct(38));
    lv_label_set_long_mode(address_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(address_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(address_label, lv_color_hex(kColorForeground), 0);
    lv_obj_align(address_label, LV_ALIGN_LEFT_MID, 6, 0);

    std::ostringstream detail;
    if (!host.mac.empty()) detail << host.mac << "  ";
    if (!host.open_ports.empty()) {
      detail << "TCP ";
      for (std::size_t i = 0; i < host.open_ports.size(); ++i) {
        if (i > 0) detail << ',';
        detail << host.open_ports[i];
      }
    } else {
      detail << "no open ports known";
    }
    if (has_review) detail << "  " LV_SYMBOL_WARNING;
    lv_obj_t* detail_label = lv_label_create(row);
    lv_label_set_text(detail_label, detail.str().c_str());
    lv_obj_set_width(detail_label, lv_pct(58));
    lv_label_set_long_mode(detail_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(detail_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(detail_label,
        lv_color_hex(has_review ? kColorAmber : kColorSecondary), 0);
    lv_obj_align(detail_label, LV_ALIGN_RIGHT_MID, -6, 0);

    attachHostAddress(row, host.address, g_recon_screen);
  }
}

void refreshFindingsList() {
  using namespace reconclave::ui;
  if (g_findings_list == nullptr) return;
  reconclave::HostKnowledge knowledge(kHostKnowledgePath);
  std::string error;
  knowledge.load(error);
  const auto findings = knowledge.findingsJson();
  const auto* items = findings.find("findings");
  const auto* count = findings.find("count");
  if (g_findings_status != nullptr) {
    const std::string status = std::to_string(count ? count->asUInt64() : 0) +
        (count && count->asUInt64() == 1 ? " finding requiring review" : " findings requiring review");
    lv_label_set_text(g_findings_status, status.c_str());
  }
  lv_obj_clean(g_findings_list);
  if (items == nullptr || !items->isArray() || items->items().empty()) {
    lv_obj_t* empty = lv_obj_create(g_findings_list);
    lv_obj_set_size(empty, lv_pct(100), 260);
    lv_obj_set_style_bg_color(empty, lv_color_hex(0x091922), 0);
    lv_obj_set_style_border_color(empty, lv_color_hex(kColorAccent), 0);
    lv_obj_set_style_border_width(empty, 1, 0);
    lv_obj_set_style_radius(empty, 10, 0);
    lv_obj_set_style_shadow_width(empty, 0, 0);
    lv_obj_set_style_pad_all(empty, 18, 0);
    lv_obj_clear_flag(empty, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(empty, LV_LAYOUT_NONE);

    lv_obj_t* icon = lv_label_create(empty);
    lv_label_set_text(icon, LV_SYMBOL_OK);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(kColorAccent), 0);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t* title = lv_label_create(empty);
    lv_label_set_text(title, "REVIEW QUEUE CLEAR");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(kColorForeground), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 52);

    lv_obj_t* detail = lv_label_create(empty);
    lv_label_set_text(detail,
        "Noteworthy services and changes to known hosts will appear here after discovery.");
    lv_obj_set_width(detail, 650);
    lv_label_set_long_mode(detail, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(detail, lv_color_hex(kColorSecondary), 0);
    lv_obj_set_style_text_font(detail, &lv_font_montserrat_14, 0);
    lv_obj_align(detail, LV_ALIGN_TOP_MID, 0, 88);

    lv_obj_t* survey = lv_button_create(empty);
    lv_obj_set_size(survey, 210, 46);
    lv_obj_align(survey, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_style_bg_color(survey, lv_color_hex(kColorPanelLight), 0);
    lv_obj_set_style_border_color(survey, lv_color_hex(kColorAmber), 0);
    lv_obj_set_style_border_width(survey, 1, 0);
    lv_obj_set_style_radius(survey, 8, 0);
    lv_obj_set_style_shadow_width(survey, 0, 0);
    lv_obj_add_event_cb(survey, [](lv_event_t*) {
      if (g_recon_screen != nullptr) lv_screen_load(g_recon_screen);
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* survey_label = lv_label_create(survey);
    lv_label_set_text(survey_label, LV_SYMBOL_REFRESH "  OPEN LAN SURVEY");
    lv_obj_set_style_text_color(survey_label, lv_color_hex(kColorAmber), 0);
    lv_obj_center(survey_label);
    return;
  }
  for (const auto& finding : items->items()) {
    const auto* severity = finding.find("severity");
    const auto* title = finding.find("title");
    const auto* address = finding.find("address");
    const auto* port = finding.find("port");
    const auto* guidance = finding.find("guidance");
    const auto* last_seen = finding.find("last_seen_ms");
    const bool review = severity != nullptr && severity->asString() == "review";

    lv_obj_t* row = makeListRow(g_findings_list, review ? kColorAmber : kColorAccent, 92);

    std::ostringstream heading;
    heading << (review ? LV_SYMBOL_WARNING "  " : LV_SYMBOL_BELL "  ")
            << (title ? title->asString() : "Finding");
    if (address != nullptr) {
      heading << "   -   " << address->asString();
      if (port != nullptr) heading << ":" << static_cast<long long>(port->asNumber());
    }
    lv_obj_t* heading_label = lv_label_create(row);
    lv_label_set_text(heading_label, heading.str().c_str());
    lv_obj_set_width(heading_label, lv_pct(100));
    lv_label_set_long_mode(heading_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(heading_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(heading_label, lv_color_hex(review ? kColorAmber : kColorForeground), 0);
    lv_obj_align(heading_label, LV_ALIGN_TOP_LEFT, 0, 0);

    std::ostringstream sub;
    if (guidance != nullptr) sub << guidance->asString();
    if (last_seen != nullptr) sub << "   |   seen " << formatEpochMs(last_seen->asUInt64());
    lv_obj_t* sub_label = lv_label_create(row);
    lv_label_set_text(sub_label, sub.str().c_str());
    lv_obj_set_width(sub_label, lv_pct(100));
    lv_label_set_long_mode(sub_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(sub_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(sub_label, lv_color_hex(kColorSecondary), 0);
    lv_obj_align(sub_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    if (address != nullptr) attachHostAddress(row, address->asString(), g_findings_screen);
  }
}

// Repopulates the Host Detail screen's labels without navigating to it -
// used both by showHostDetail() (which then navigates) and by the periodic
// refresh timer (so a host's detail screen updates live while the operator
// is actually looking at it, without yanking them back to it otherwise).
void populateHostDetail(const std::string& address) {
  if (g_host_detail_title == nullptr) return;
  reconclave::HostKnowledge knowledge(kHostKnowledgePath);
  std::string error;
  knowledge.load(error);
  const auto hosts = knowledge.snapshot();
  const auto it = std::find_if(hosts.begin(), hosts.end(),
      [&](const auto& host) { return host.address == address; });

  std::ostringstream title;
  title << address;
  if (it != hosts.end() && !it->mac.empty()) title << "   " << it->mac;
  lv_label_set_text(g_host_detail_title, title.str().c_str());

  std::ostringstream body;
  if (it == hosts.end()) {
    body << "Host record not found - it may have aged out of the local database.";
  } else {
    body << "First seen   " << formatEpochMs(it->first_seen_ms) << "\n";
    body << "Last seen    " << formatEpochMs(it->last_seen_ms) << "\n";
    body << "Open ports   ";
    if (it->open_ports.empty()) {
      body << "none known (run Survey LAN first)";
    } else {
      for (std::size_t i = 0; i < it->open_ports.size(); ++i) {
        if (i > 0) body << ',';
        body << it->open_ports[i];
      }
    }
    body << "\n\n";
    if (it->services.empty()) {
      body << "No service identification yet. Tap Re-check ports below.";
    } else {
      for (const auto& service : it->services) {
        body << "PORT " << service.port << "   " << service.service;
        if (!service.banner.empty()) body << "   \"" << service.banner << "\"";
        body << "\n";
        if (!service.attention.empty()) body << "  " LV_SYMBOL_WARNING "  " << service.attention << "\n";
      }
    }
  }
  lv_label_set_text(g_host_detail_body, body.str().c_str());

  const bool can_recheck = it != hosts.end() && !it->open_ports.empty();
  if (g_host_detail_recheck_button != nullptr) {
    if (can_recheck) lv_obj_clear_state(g_host_detail_recheck_button, LV_STATE_DISABLED);
    else lv_obj_add_state(g_host_detail_recheck_button, LV_STATE_DISABLED);
  }
}

void showHostDetail(const std::string& address, lv_obj_t* return_screen) {
  if (g_host_detail_screen == nullptr) return;
  g_host_detail_address = address;
  g_host_detail_return = return_screen;
  populateHostDetail(address);
  if (g_host_detail_status != nullptr) lv_label_set_text(g_host_detail_status, "");
  lv_screen_load(g_host_detail_screen);
}

std::string readDeviceInfo() {
  utsname uts{};
  uname(&uts);

  std::ifstream meminfo("/proc/meminfo");
  std::string line, mem_total, mem_available;
  while (std::getline(meminfo, line)) {
    if (line.rfind("MemTotal:", 0) == 0) mem_total = line.substr(9);
    if (line.rfind("MemAvailable:", 0) == 0) mem_available = line.substr(13);
  }

  const auto memory_mb = [](const std::string& kb) {
    return std::strtoull(kb.c_str(), nullptr, 10) / 1024;
  };
  std::ostringstream out;
  out << "SYSTEM IDENTITY\n\n"
      << "DEVICE ID\n" << kDeviceId << "\n\n"
      << "FIRMWARE\n" << kFirmware << "\n\n"
      << "HOSTNAME\n" << uts.nodename << "\n\n"
      << "KERNEL\n" << uts.sysname << " " << uts.release << "\n\n"
      << "MEMORY\n" << memory_mb(mem_available) << " MB available  //  "
      << memory_mb(mem_total) << " MB total\n\n";

  const double uptime_seconds = std::strtod(readFirstLine("/proc/uptime").c_str(), nullptr);
  out << "UPTIME\n" << static_cast<long>(uptime_seconds / 3600.0) << "h "
      << (static_cast<long>(uptime_seconds / 60.0) % 60) << "m\n";
  struct statvfs fs {};
  if (statvfs("/", &fs) == 0) {
    const uint64_t total = static_cast<uint64_t>(fs.f_blocks) * fs.f_frsize;
    const uint64_t free = static_cast<uint64_t>(fs.f_bavail) * fs.f_frsize;
    out << "\n---ACCESS---\nACCESS & SERVICES\n\nSTORAGE\n"
        << formatBytes(total - free) << " used  //  " << formatBytes(total) << " total";
  }
  out << "\n\nSSH SERVER\n"
      << (readFirstLine("/var/run/sshd.pid").empty() ? LV_SYMBOL_CLOSE "  STOPPED"
                                                     : LV_SYMBOL_OK "  READY  //  port 22  //  key only");
  out << "\n\nREMOTE CLIENTS\nSSH " << (access("/usr/bin/ssh", X_OK) == 0 ? "available" : "missing")
      << "  //  SCP & SFTP "
      << ((access("/usr/bin/scp", X_OK) == 0 && access("/usr/bin/sftp", X_OK) == 0) ? "available" : "missing");
  return out.str();
}

void refreshDeviceDetails() {
  const std::string all = readDeviceInfo();
  const std::string marker = "\n---ACCESS---\n";
  const auto split = all.find(marker);
  const std::string system = split == std::string::npos ? all : all.substr(0, split);
  lv_label_set_text(g_device_details, system.c_str());
  if (g_device_services != nullptr) {
    const std::string access = split == std::string::npos
        ? "ACCESS DATA UNAVAILABLE" : all.substr(split + marker.size());
    lv_label_set_text(g_device_services, access.c_str());
  }
}

lv_obj_t* buildReconScreen(lv_obj_t* home) {
  using namespace reconclave::ui;
  lv_obj_t* screen = createScreen();
  addStatusBar(screen, "RECON", true);
  addBackButton(screen, home);

  // Same row as the back button, right after it - Survey/Cancel stay on
  // the opposite (right) side of the row, unchanged. g_findings_screen is
  // read at click time (via the lambda body, not a captured/bound value),
  // since Findings is built after RECON in main() - by the time anyone can
  // actually tap this button, both screens exist.
  lv_obj_t* findings_button = lv_button_create(screen);
  lv_obj_set_size(findings_button, 130, kBackButtonHeight);
  lv_obj_align(findings_button, LV_ALIGN_TOP_LEFT, kSafeMargin + 84 + 12,
               kSafeMargin + kStatusBarHeight + 8);
  lv_obj_set_style_bg_color(findings_button, lv_color_hex(kColorPanelLight), 0);
  lv_obj_set_style_border_color(findings_button, lv_color_hex(kColorAmber), 0);
  lv_obj_set_style_border_width(findings_button, 1, 0);
  lv_obj_set_style_radius(findings_button, 8, 0);
  lv_obj_set_style_shadow_width(findings_button, 0, 0);
  lv_obj_add_event_cb(findings_button, [](lv_event_t*) {
    g_findings_return = g_recon_screen;
    if (g_findings_screen != nullptr) lv_screen_load(g_findings_screen);
  }, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* findings_label = lv_label_create(findings_button);
  lv_label_set_text(findings_label, LV_SYMBOL_WARNING " Findings");
  lv_obj_set_style_text_color(findings_label, lv_color_hex(kColorAmber), 0);
  lv_obj_center(findings_label);

  lv_obj_t* survey_button = lv_button_create(screen);
  lv_obj_set_size(survey_button, 170, kBackButtonHeight);
  lv_obj_align(survey_button, LV_ALIGN_TOP_RIGHT, -kSafeMargin - 130,
               kSafeMargin + kStatusBarHeight + 8);
  lv_obj_set_style_bg_color(survey_button, lv_color_hex(kColorAccent), 0);
  lv_obj_set_style_radius(survey_button, 8, 0);
  lv_obj_set_style_shadow_width(survey_button, 0, 0);
  lv_obj_add_event_cb(survey_button, [](lv_event_t*) { requestLanSurvey(); }, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* survey_label = lv_label_create(survey_button);
  lv_label_set_text(survey_label, LV_SYMBOL_REFRESH " Survey LAN");
  lv_obj_set_style_text_color(survey_label, lv_color_hex(kColorPanel), 0);
  lv_obj_center(survey_label);

  lv_obj_t* cancel_button = lv_button_create(screen);
  lv_obj_set_size(cancel_button, 118, kBackButtonHeight);
  lv_obj_align(cancel_button, LV_ALIGN_TOP_RIGHT, -kSafeMargin,
               kSafeMargin + kStatusBarHeight + 8);
  lv_obj_set_style_bg_color(cancel_button, lv_color_hex(kColorPanelLight), 0);
  lv_obj_set_style_border_color(cancel_button, lv_color_hex(kColorAmber), 0);
  lv_obj_set_style_border_width(cancel_button, 1, 0);
  lv_obj_set_style_radius(cancel_button, 8, 0);
  lv_obj_set_style_shadow_width(cancel_button, 0, 0);
  lv_obj_add_event_cb(cancel_button, [](lv_event_t*) { cancelLanSurvey(); }, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* cancel_label = lv_label_create(cancel_button);
  lv_label_set_text(cancel_label, LV_SYMBOL_CLOSE " Cancel");
  lv_obj_set_style_text_color(cancel_label, lv_color_hex(kColorAmber), 0);
  lv_obj_center(cancel_label);

  constexpr lv_coord_t kSummaryHeight = 60;
  constexpr lv_coord_t kActionStatusHeight = 24;
  constexpr lv_coord_t kGap = 8;

  lv_obj_t* card = lv_obj_create(screen);
  lv_obj_set_size(card, contentWidth(screen), kSummaryHeight);
  lv_obj_align(card, LV_ALIGN_TOP_LEFT, kSafeMargin, kContentTop);
  lv_obj_set_style_bg_color(card, lv_color_hex(kColorPanelLight), 0);
  lv_obj_set_style_border_color(card, lv_color_hex(kColorAccent), 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_radius(card, 12, 0);
  lv_obj_set_style_pad_all(card, 14, 0);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

  g_recon_details = lv_label_create(card);
  std::string detail = reconDashboardText();
  lv_label_set_text(g_recon_details, detail.c_str());
  lv_obj_set_width(g_recon_details, lv_pct(100));
  lv_label_set_long_mode(g_recon_details, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_style_text_color(g_recon_details, lv_color_hex(kColorForeground), 0);
  lv_obj_set_style_text_font(g_recon_details, &lv_font_montserrat_16, 0);
  lv_obj_align(g_recon_details, LV_ALIGN_TOP_LEFT, 0, 0);

  g_recon_action_status = lv_label_create(screen);
  lv_label_set_text(g_recon_action_status, "Observe mode  |  Survey requires two-tap confirmation  |  tap a host below for detail");
  lv_obj_set_width(g_recon_action_status, contentWidth(screen));
  lv_label_set_long_mode(g_recon_action_status, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_style_text_color(g_recon_action_status, lv_color_hex(kColorSecondary), 0);
  lv_obj_set_style_text_font(g_recon_action_status, &lv_font_montserrat_14, 0);
  lv_obj_align(g_recon_action_status, LV_ALIGN_TOP_LEFT, kSafeMargin, kContentTop + kSummaryHeight + kGap);

  g_recon_host_list = lv_list_create(screen);
  const lv_coord_t list_top = kContentTop + kSummaryHeight + kGap + kActionStatusHeight + kGap;
  lv_obj_set_size(g_recon_host_list, contentWidth(screen), contentHeight(screen) - kSummaryHeight -
                   kActionStatusHeight - 2 * kGap);
  lv_obj_align(g_recon_host_list, LV_ALIGN_TOP_MID, 0, list_top);
  lv_obj_set_style_bg_color(g_recon_host_list, lv_color_hex(kColorPanelLight), 0);
  lv_obj_set_style_radius(g_recon_host_list, 10, 0);
  lv_obj_set_style_border_width(g_recon_host_list, 0, 0);
  refreshReconHostList();

  g_recon_screen = screen;
  return screen;
}

lv_obj_t* buildFindingsScreen(lv_obj_t* home) {
  using namespace reconclave::ui;
  lv_obj_t* screen = createScreen();
  addStatusBar(screen, "FINDINGS", true);
  // Opened only from RECON, so back returns there - not the fixed `home`
  // addBackButton() always uses (see addDynamicBackButton's comment).
  addDynamicBackButton(screen, &g_findings_return, home);

  g_findings_status = lv_label_create(screen);
  lv_label_set_text(g_findings_status, "0 findings requiring review");
  lv_obj_set_style_text_color(g_findings_status, lv_color_hex(kColorSecondary), 0);
  lv_obj_set_style_text_font(g_findings_status, &lv_font_montserrat_16, 0);
  lv_obj_align(g_findings_status, LV_ALIGN_TOP_LEFT, kSafeMargin, kContentTop);

  g_findings_list = lv_list_create(screen);
  constexpr lv_coord_t kStatusHeight = 24;
  constexpr lv_coord_t kGap = 8;
  lv_obj_set_size(g_findings_list, contentWidth(screen), contentHeight(screen) - kStatusHeight - kGap);
  lv_obj_align(g_findings_list, LV_ALIGN_TOP_MID, 0, kContentTop + kStatusHeight + kGap);
  lv_obj_set_style_bg_color(g_findings_list, lv_color_hex(kColorPanelLight), 0);
  lv_obj_set_style_radius(g_findings_list, 10, 0);
  lv_obj_set_style_border_width(g_findings_list, 0, 0);
  refreshFindingsList();

  g_findings_screen = screen;
  return screen;
}

lv_obj_t* buildHostDetailScreen(lv_obj_t* home) {
  using namespace reconclave::ui;
  lv_obj_t* screen = createScreen();
  addStatusBar(screen, "HOST DETAIL", true);
  // Opened from either RECON's host list or Findings - back returns to
  // whichever one it actually was (see attachHostAddress/showHostDetail),
  // falling back to `home` only if none was recorded.
  addDynamicBackButton(screen, &g_host_detail_return, home);

  g_host_detail_recheck_button = lv_button_create(screen);
  lv_obj_set_size(g_host_detail_recheck_button, 190, kBackButtonHeight);
  lv_obj_align(g_host_detail_recheck_button, LV_ALIGN_TOP_RIGHT, -kSafeMargin,
               kSafeMargin + kStatusBarHeight + 8);
  lv_obj_set_style_bg_color(g_host_detail_recheck_button, lv_color_hex(kColorAccent), 0);
  lv_obj_set_style_bg_color(g_host_detail_recheck_button, lv_color_hex(kColorUnavailable), LV_STATE_DISABLED);
  lv_obj_set_style_radius(g_host_detail_recheck_button, 8, 0);
  lv_obj_set_style_shadow_width(g_host_detail_recheck_button, 0, 0);
  lv_obj_add_event_cb(g_host_detail_recheck_button, [](lv_event_t*) {
    if (g_host_detail_address.empty()) return;
    reconclave::HostKnowledge knowledge(kHostKnowledgePath);
    std::string error;
    knowledge.load(error);
    const auto hosts = knowledge.snapshot();
    const auto it = std::find_if(hosts.begin(), hosts.end(),
        [&](const auto& host) { return host.address == g_host_detail_address; });
    if (it == hosts.end() || it->open_ports.empty()) {
      lv_label_set_text(g_host_detail_status, "No known open ports to re-check - run Survey LAN first.");
      return;
    }
    std::ostringstream ports;
    for (std::size_t i = 0; i < it->open_ports.size(); ++i) {
      if (i > 0) ports << ',';
      ports << it->open_ports[i];
    }
    const std::string command = "IDENTIFY " + g_host_detail_address + " " + ports.str();
    lv_label_set_text(g_host_detail_status, sendLocalCommand(command)
        ? "Re-check requested; results appear here within a few seconds."
        : "Unable to contact the local node service.");
  }, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* recheck_label = lv_label_create(g_host_detail_recheck_button);
  lv_label_set_text(recheck_label, LV_SYMBOL_REFRESH " Re-check ports");
  lv_obj_set_style_text_color(recheck_label, lv_color_hex(kColorPanel), 0);
  lv_obj_center(recheck_label);

  g_host_detail_title = lv_label_create(screen);
  lv_label_set_text(g_host_detail_title, "");
  lv_obj_set_width(g_host_detail_title, contentWidth(screen));
  lv_label_set_long_mode(g_host_detail_title, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_style_text_color(g_host_detail_title, lv_color_hex(kColorForeground), 0);
  lv_obj_set_style_text_font(g_host_detail_title, &lv_font_montserrat_22, 0);
  lv_obj_align(g_host_detail_title, LV_ALIGN_TOP_LEFT, kSafeMargin, kContentTop);

  lv_obj_t* card = lv_obj_create(screen);
  lv_obj_set_size(card, contentWidth(screen), contentHeight(screen) - 44 - 30);
  lv_obj_align(card, LV_ALIGN_TOP_LEFT, kSafeMargin, kContentTop + 44);
  lv_obj_set_style_bg_color(card, lv_color_hex(kColorPanelLight), 0);
  lv_obj_set_style_border_width(card, 0, 0);
  lv_obj_set_style_radius(card, 12, 0);
  lv_obj_set_style_pad_all(card, 18, 0);

  g_host_detail_body = lv_label_create(card);
  lv_label_set_text(g_host_detail_body, "");
  lv_obj_set_width(g_host_detail_body, lv_pct(100));
  lv_label_set_long_mode(g_host_detail_body, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_style_text_color(g_host_detail_body, lv_color_hex(kColorForeground), 0);
  lv_obj_set_style_text_font(g_host_detail_body, &lv_font_montserrat_16, 0);
  lv_obj_align(g_host_detail_body, LV_ALIGN_TOP_LEFT, 0, 0);

  g_host_detail_status = lv_label_create(screen);
  lv_label_set_text(g_host_detail_status, "");
  lv_obj_set_width(g_host_detail_status, contentWidth(screen));
  lv_label_set_long_mode(g_host_detail_status, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_style_text_color(g_host_detail_status, lv_color_hex(kColorSecondary), 0);
  lv_obj_set_style_text_font(g_host_detail_status, &lv_font_montserrat_14, 0);
  lv_obj_align(g_host_detail_status, LV_ALIGN_BOTTOM_LEFT, kSafeMargin, -kSafeMargin);

  g_host_detail_screen = screen;
  return screen;
}

lv_obj_t* buildVisionScreen(lv_obj_t* home) {
  using namespace reconclave::ui;
  lv_obj_t* screen = createScreen();
  addStatusBar(screen, "VISION", true);
  addBackButton(screen, home);

  const bool camera_ready = pathExists(kCameraDevice);

  lv_obj_t* capture_button = lv_button_create(screen);
  lv_obj_set_size(capture_button, 170, kBackButtonHeight);
  lv_obj_align(capture_button, LV_ALIGN_TOP_RIGHT, -kSafeMargin, kSafeMargin + kStatusBarHeight + 8);
  lv_obj_set_style_bg_color(capture_button, lv_color_hex(kColorAmber), 0);
  lv_obj_set_style_bg_color(capture_button, lv_color_hex(kColorUnavailable), LV_STATE_DISABLED);
  lv_obj_set_style_radius(capture_button, 8, 0);
  lv_obj_set_style_shadow_width(capture_button, 0, 0);
  if (!camera_ready) lv_obj_add_state(capture_button, LV_STATE_DISABLED);
  lv_obj_add_event_cb(capture_button, [](lv_event_t*) { captureEvidencePhoto(); },
                      LV_EVENT_CLICKED, nullptr);
  lv_obj_t* capture_label = lv_label_create(capture_button);
  lv_label_set_text(capture_label, LV_SYMBOL_IMAGE " Capture photo");
  lv_obj_set_style_text_color(capture_label, lv_color_hex(kColorPanel), 0);
  lv_obj_center(capture_label);

  g_vision_preview_button = lv_button_create(screen);
  lv_obj_set_size(g_vision_preview_button, 160, kBackButtonHeight);
  lv_obj_align(g_vision_preview_button, LV_ALIGN_TOP_RIGHT, -kSafeMargin - 170 - 10,
               kSafeMargin + kStatusBarHeight + 8);
  lv_obj_set_style_bg_color(g_vision_preview_button, lv_color_hex(kColorPanelLight), 0);
  lv_obj_set_style_border_color(g_vision_preview_button, lv_color_hex(kColorAccent), 0);
  lv_obj_set_style_border_width(g_vision_preview_button, 1, 0);
  lv_obj_set_style_radius(g_vision_preview_button, 8, 0);
  lv_obj_set_style_shadow_width(g_vision_preview_button, 0, 0);
  if (!camera_ready) lv_obj_add_state(g_vision_preview_button, LV_STATE_DISABLED);
  lv_obj_add_event_cb(g_vision_preview_button, [](lv_event_t*) { togglePreview(); },
                      LV_EVENT_CLICKED, nullptr);
  g_vision_preview_button_label = lv_label_create(g_vision_preview_button);
  lv_label_set_text(g_vision_preview_button_label, LV_SYMBOL_PLAY " Start preview");
  lv_obj_set_style_text_color(g_vision_preview_button_label, lv_color_hex(kColorAccent), 0);
  lv_obj_center(g_vision_preview_button_label);

  // The viewfinder owns most of the screen. Operational information sits
  // in a narrow rail so camera framing is useful rather than thumbnail-sized.
  constexpr lv_coord_t kInfoWidth = 340;
  constexpr lv_coord_t kGap = 16;
  const lv_coord_t preview_width = contentWidth(screen) - kInfoWidth - kGap;
  const lv_coord_t panel_height = contentHeight(screen);

  lv_obj_t* preview_panel = lv_obj_create(screen);
  lv_obj_set_size(preview_panel, preview_width, panel_height);
  lv_obj_align(preview_panel, LV_ALIGN_TOP_LEFT, kSafeMargin, kContentTop);
  lv_obj_set_style_bg_color(preview_panel, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_color(preview_panel, lv_color_hex(kColorAccent), 0);
  lv_obj_set_style_border_width(preview_panel, 1, 0);
  lv_obj_set_style_radius(preview_panel, 12, 0);
  lv_obj_set_style_pad_all(preview_panel, 4, 0);
  lv_obj_clear_flag(preview_panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(preview_panel, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(preview_panel, [](lv_event_t*) { openPhotoViewer(); }, LV_EVENT_CLICKED, nullptr);

  g_vision_preview = lv_image_create(preview_panel);
  lv_obj_center(g_vision_preview);
  g_vision_preview_hint = lv_label_create(preview_panel);
  lv_label_set_text(g_vision_preview_hint,
                    LV_SYMBOL_IMAGE "  VIEWFINDER READY\n\nTap Start preview to frame the shot");
  lv_obj_set_width(g_vision_preview_hint, lv_pct(90));
  lv_obj_set_style_text_align(g_vision_preview_hint, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(g_vision_preview_hint, lv_color_hex(kColorSecondary), 0);
  lv_obj_set_style_text_font(g_vision_preview_hint, &lv_font_montserrat_16, 0);
  lv_obj_center(g_vision_preview_hint);

  lv_obj_t* card = lv_obj_create(screen);
  lv_obj_set_size(card, kInfoWidth, panel_height);
  lv_obj_align(card, LV_ALIGN_TOP_RIGHT, -kSafeMargin, kContentTop);
  lv_obj_set_style_bg_color(card, lv_color_hex(kColorPanelLight), 0);
  lv_obj_set_style_border_color(card, lv_color_hex(kColorAmber), 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_radius(card, 12, 0);
  lv_obj_set_style_pad_all(card, 18, 0);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

  g_vision_details = lv_label_create(card);
  std::ostringstream text;
  text << (camera_ready ? LV_SYMBOL_OK "  CAMERA ONLINE" : LV_SYMBOL_WARNING "  CAMERA OFFLINE")
       << "\nGC2093  //  1920 x 1080\n\n"
       << (camera_ready
           ? "CAPTURE READY\nPreview frames are temporary. Capture saves a timestamped evidence JPEG."
           : "No camera capture device is currently available.")
       << "\n\nTap the image to inspect it full-screen.\n\nSTATUS";
  lv_label_set_text(g_vision_details, text.str().c_str());
  lv_obj_set_width(g_vision_details, lv_pct(100));
  lv_label_set_long_mode(g_vision_details, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_style_text_color(g_vision_details, lv_color_hex(kColorForeground), 0);
  lv_obj_set_style_text_font(g_vision_details, &lv_font_montserrat_14, 0);
  lv_obj_align(g_vision_details, LV_ALIGN_TOP_LEFT, 0, 0);

  g_vision_status = lv_label_create(card);
  const std::string initial_status = camera_ready
      ? "READY  //  Tap Capture photo to create a timestamped JPEG evidence artifact."
      : "No camera device found at " + std::string(kCameraDevice);
  lv_label_set_text(g_vision_status, initial_status.c_str());
  lv_obj_set_width(g_vision_status, lv_pct(100));
  lv_label_set_long_mode(g_vision_status, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_style_text_color(g_vision_status, lv_color_hex(kColorSecondary), 0);
  lv_obj_set_style_text_font(g_vision_status, &lv_font_montserrat_12, 0);
  lv_obj_align(g_vision_status, LV_ALIGN_BOTTOM_LEFT, 0, 0);

  // The actual capture work happens on a background thread (see
  // previewWorkerLoop) - this timer just polls for a new frame cheaply on
  // the UI thread, and only runs while VISION is actually the loaded
  // screen (LOADED/UNLOADED fire on lv_screen_load(), so this naturally
  // stops polling when the operator navigates away or opens the
  // full-screen viewer, and resumes on return).
  //
  // Not auto-started on screen load for now: the on-screen thumbnail is a
  // known-broken/parked issue (see the warning in g_vision_details above),
  // so there's no point continuously exercising the camera/ISP driver in
  // the background for a feature that doesn't render anything - especially
  // right after this same repeated-cycling pattern was the leading
  // suspect in an earlier full-device hang (see README's Vision section).
  // Capture photo (captureEvidencePhoto(), a single deliberate shot) is
  // unaffected and still fully works.
  if (camera_ready) {
    g_vision_preview_timer = lv_timer_create(checkPreviewWorker, 150, nullptr);
    lv_timer_pause(g_vision_preview_timer);
    lv_obj_add_event_cb(screen, [](lv_event_t*) {
      if (g_vision_preview_timer != nullptr) lv_timer_pause(g_vision_preview_timer);
      stopPreviewWorker();
      updatePreviewButton(false);
    }, LV_EVENT_SCREEN_UNLOADED, nullptr);
  }

  g_vision_screen = screen;
  return screen;
}

lv_obj_t* buildPhotoViewerScreen(lv_obj_t* home) {
  using namespace reconclave::ui;
  lv_obj_t* screen = createScreen();
  addStatusBar(screen, "PHOTO", true);
  // Only ever opened from VISION's preview panel.
  addDynamicBackButton(screen, &g_vision_screen, home);

  constexpr lv_coord_t kCaptionHeight = 24;
  constexpr lv_coord_t kGap = 6;
  const lv_coord_t frame_height = contentHeight(screen) - kCaptionHeight - kGap;

  lv_obj_t* frame = lv_obj_create(screen);
  lv_obj_set_size(frame, contentWidth(screen), frame_height);
  lv_obj_align(frame, LV_ALIGN_TOP_MID, 0, kContentTop);
  lv_obj_set_style_bg_color(frame, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_width(frame, 0, 0);
  lv_obj_set_style_radius(frame, 10, 0);
  lv_obj_clear_flag(frame, LV_OBJ_FLAG_SCROLLABLE);

  g_photo_viewer_image = lv_image_create(frame);

  g_photo_viewer_caption = lv_label_create(screen);
  lv_label_set_text(g_photo_viewer_caption, "");
  lv_obj_set_width(g_photo_viewer_caption, contentWidth(screen));
  lv_label_set_long_mode(g_photo_viewer_caption, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_style_text_color(g_photo_viewer_caption, lv_color_hex(kColorSecondary), 0);
  lv_obj_set_style_text_font(g_photo_viewer_caption, &lv_font_montserrat_14, 0);
  lv_obj_align(g_photo_viewer_caption, LV_ALIGN_TOP_LEFT, kSafeMargin, kContentTop + frame_height + kGap);

  g_photo_viewer_screen = screen;
  return screen;
}

lv_obj_t* buildLocationScreen(lv_obj_t* home) {
  using namespace reconclave::ui;
  lv_obj_t* screen = createScreen();
  addStatusBar(screen, "LOCATION", true);
  addBackButton(screen, home);

  lv_obj_t* start_button = lv_button_create(screen);
  lv_obj_set_size(start_button, 140, kBackButtonHeight);
  lv_obj_align(start_button, LV_ALIGN_TOP_RIGHT, -kSafeMargin, kSafeMargin + kStatusBarHeight + 8);
  lv_obj_set_style_bg_color(start_button, lv_color_hex(kColorPanelLight), 0);
  lv_obj_set_style_border_color(start_button, lv_color_hex(kColorAccent), 0);
  lv_obj_set_style_border_width(start_button, 1, 0);
  lv_obj_set_style_shadow_width(start_button, 0, 0);
  lv_obj_set_style_radius(start_button, 8, 0);
  lv_obj_add_event_cb(start_button, [](lv_event_t*) {
    std::string error;
    if (!g_gnss.running()) {
      if (!enableNrf9151()) error = "unable to enable nRF9151 on GPIO2";
      else g_gnss.start("/dev/ttyS3", error);
    }
    if (!error.empty() && g_gnss_details != nullptr) {
      lv_label_set_text(g_gnss_details, error.c_str());
      return;
    }
    refreshLiveDetails(nullptr);
  }, LV_EVENT_CLICKED, nullptr);
  if (!kGnssAvailable) lv_obj_add_state(start_button, LV_STATE_DISABLED);
  lv_obj_t* start_label = lv_label_create(start_button);
  lv_label_set_text(start_label, kGnssAvailable ? LV_SYMBOL_PLAY " Start GNSS"
                                                : LV_SYMBOL_CLOSE " GNSS unavailable");
  lv_obj_set_style_text_color(start_label, lv_color_hex(kColorAccent), 0);
  lv_obj_center(start_label);

  lv_obj_t* card = lv_obj_create(screen);
  lv_obj_set_size(card, contentWidth(screen), contentHeight(screen));
  lv_obj_align(card, LV_ALIGN_TOP_LEFT, kSafeMargin, kContentTop);
  lv_obj_set_style_bg_color(card, lv_color_hex(kColorPanelLight), 0);
  lv_obj_set_style_border_color(card, lv_color_hex(kColorAccent), 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_radius(card, 12, 0);
  lv_obj_set_style_pad_all(card, 18, 0);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  g_gnss_details = lv_label_create(card);
  lv_obj_set_width(g_gnss_details, lv_pct(100));
  lv_label_set_long_mode(g_gnss_details, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_style_text_color(g_gnss_details, lv_color_hex(kColorForeground), 0);
  lv_obj_set_style_text_font(g_gnss_details, &lv_font_montserrat_16, 0);
  lv_obj_align(g_gnss_details, LV_ALIGN_TOP_LEFT, 0, 0);
  return screen;
}

lv_obj_t* buildAssessmentScreen(lv_obj_t* home) {
  using namespace reconclave::ui;
  lv_obj_t* screen = createScreen();
  addStatusBar(screen, "ASSESSMENT", true);
  addBackButton(screen, home);

  lv_obj_t* save_button = lv_button_create(screen);
  lv_obj_set_size(save_button, 110, kBackButtonHeight);
  lv_obj_align(save_button, LV_ALIGN_TOP_RIGHT, -kSafeMargin, kSafeMargin + kStatusBarHeight + 8);
  lv_obj_set_style_bg_color(save_button, lv_color_hex(kColorPanelLight), 0);
  lv_obj_set_style_border_color(save_button, lv_color_hex(kColorAccent), 0);
  lv_obj_set_style_border_width(save_button, 1, 0);
  lv_obj_set_style_shadow_width(save_button, 0, 0);

  lv_obj_t* keyboard = nullptr;
  if (!g_keyboard_ready) {
    keyboard = lv_keyboard_create(screen);
    lv_obj_set_size(keyboard, contentWidth(screen), 200);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, -kSafeMargin);
    lv_obj_set_style_bg_color(keyboard, lv_color_hex(kColorPanel), 0);
  }

  const char* names[] = {"PROJECT ID", "ENGAGEMENT", "OPERATOR"};
  const std::string* values[] = {&g_project_id, &g_engagement_id, &g_operator_id};
  lv_obj_t* fields[3]{};
  const int field_width = (contentWidth(screen) - 32) / 3;
  for (int i = 0; i < 3; ++i) {
    lv_obj_t* label = lv_label_create(screen);
    lv_label_set_text(label, names[i]);
    lv_obj_set_style_text_color(label, lv_color_hex(kColorSecondary), 0);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, kSafeMargin + i * (field_width + 16), kContentTop);
    fields[i] = lv_textarea_create(screen);
    lv_obj_set_size(fields[i], field_width, 64);
    lv_obj_align(fields[i], LV_ALIGN_TOP_LEFT, kSafeMargin + i * (field_width + 16), kContentTop + 28);
    lv_textarea_set_one_line(fields[i], true);
    lv_textarea_set_max_length(fields[i], 48);
    lv_textarea_set_text(fields[i], values[i]->c_str());
    lv_obj_set_style_bg_color(fields[i], lv_color_hex(kColorPanelLight), 0);
    lv_obj_set_style_text_color(fields[i], lv_color_hex(kColorForeground), 0);
    lv_obj_set_style_border_color(fields[i], lv_color_hex(kColorAccent), LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(fields[i], 1, 0);
    lv_obj_set_style_border_width(fields[i], 2, LV_STATE_FOCUSED);
    lv_obj_set_style_radius(fields[i], 9, 0);
    lv_obj_set_style_shadow_width(fields[i], 0, 0);
    lv_obj_add_event_cb(fields[i], [](lv_event_t* event) {
      auto* keyboard = static_cast<lv_obj_t*>(lv_event_get_user_data(event));
      if (keyboard != nullptr) {
        lv_keyboard_set_textarea(keyboard, static_cast<lv_obj_t*>(lv_event_get_target(event)));
      }
    }, LV_EVENT_FOCUSED, keyboard);
  }
  if (keyboard != nullptr) lv_keyboard_set_textarea(keyboard, fields[0]);

  struct SessionFields { lv_obj_t* project; lv_obj_t* engagement; lv_obj_t* operator_id; };
  auto* session_fields = new SessionFields{fields[0], fields[1], fields[2]};
  lv_obj_add_event_cb(save_button, [](lv_event_t* event) {
    auto* entry = static_cast<SessionFields*>(lv_event_get_user_data(event));
    g_project_id = safeSessionValue(lv_textarea_get_text(entry->project), "UNASSIGNED");
    g_engagement_id = safeSessionValue(lv_textarea_get_text(entry->engagement), "UNASSIGNED");
    g_operator_id = safeSessionValue(lv_textarea_get_text(entry->operator_id), "LOCAL");
    lv_label_set_text(g_session_status, saveSession() ? "Session saved; new evidence will carry this context"
                                                      : "Unable to save session");
  }, LV_EVENT_CLICKED, session_fields);
  lv_obj_t* save_label = lv_label_create(save_button);
  lv_label_set_text(save_label, LV_SYMBOL_SAVE " Save");
  lv_obj_set_style_text_color(save_label, lv_color_hex(kColorAccent), 0);
  lv_obj_center(save_label);

  lv_obj_t* activity = lv_obj_create(screen);
  // Vendor textarea styling enforces a taller rendered box than the nominal
  // object height. Keep the activity panel below that real 112 px footprint.
  constexpr lv_coord_t kFieldsFootprint = 150;
  const lv_coord_t activity_top = kContentTop + kFieldsFootprint;
  const lv_coord_t activity_height = contentHeight(screen) - kFieldsFootprint -
                                     (keyboard != nullptr ? 212 : 0);
  lv_obj_set_size(activity, contentWidth(screen), activity_height);
  lv_obj_align(activity, LV_ALIGN_TOP_LEFT, kSafeMargin, activity_top);
  lv_obj_set_style_bg_color(activity, lv_color_hex(0x091922), 0);
  lv_obj_set_style_border_color(activity, lv_color_hex(kColorAccent), 0);
  lv_obj_set_style_border_width(activity, 1, 0);
  lv_obj_set_style_radius(activity, 10, 0);
  lv_obj_set_style_pad_all(activity, 18, 0);
  lv_obj_clear_flag(activity, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t* activity_heading = lv_label_create(activity);
  lv_label_set_text(activity_heading, "ASSESSMENT CONTEXT  //  EVIDENCE ATTRIBUTION");
  lv_obj_set_style_text_color(activity_heading, lv_color_hex(kColorAccent), 0);
  lv_obj_set_style_text_font(activity_heading, &lv_font_montserrat_12, 0);
  lv_obj_align(activity_heading, LV_ALIGN_TOP_LEFT, 0, 0);

  g_session_status = lv_label_create(activity);
  lv_label_set_text(g_session_status, g_keyboard_ready
      ? "HARDWARE KEYBOARD READY\n\nSelect a field, enter the engagement context, then tap Save.\n"
        "New evidence artifacts will inherit these identifiers."
      : "TOUCH INPUT READY\n\nSet field context before collecting assessment evidence.");
  lv_obj_set_style_text_color(g_session_status, lv_color_hex(kColorSecondary), 0);
  lv_obj_set_style_text_font(g_session_status, &lv_font_montserrat_14, 0);
  lv_obj_set_width(g_session_status, lv_pct(100));
  lv_label_set_long_mode(g_session_status, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_align(g_session_status, LV_ALIGN_TOP_LEFT, 0, 34);
  return screen;
}

lv_obj_t* buildNodeScreen(lv_obj_t* home) {
  using namespace reconclave::ui;
  lv_obj_t* screen = createScreen();
  addStatusBar(screen, "NODE & JOBS", true);
  addBackButton(screen, home);
  constexpr lv_coord_t kGap = 12;
  const lv_coord_t panel_width = (contentWidth(screen) - 2 * kGap) / 3;
  auto make_panel = [&](int index, std::uint32_t border) {
    lv_obj_t* card = lv_obj_create(screen);
    lv_obj_set_size(card, panel_width, contentHeight(screen));
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, kSafeMargin + index * (panel_width + kGap), kContentTop);
    lv_obj_set_style_bg_color(card, lv_color_hex(index == 1 ? 0x091922 : kColorPanelLight), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(border), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 18, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* label = lv_label_create(card);
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_color(label, lv_color_hex(kColorForeground), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);
    return label;
  };
  g_node_details = make_panel(0, kColorAccent);
  g_node_trust = make_panel(1, kColorAmber);
  g_node_job = make_panel(2, kColorAccent);
  refreshNodeDetails();
  return screen;
}

lv_obj_t* buildWirelessScreen(lv_obj_t* home) {
  using namespace reconclave::ui;
  lv_obj_t* screen = createScreen();
  addStatusBar(screen, "WIRELESS", true);
  addBackButton(screen, home);

  // Same row as the back button, right-aligned, rather than overlapping
  // the status bar above it.
  lv_obj_t* scan_button = lv_button_create(screen);
  lv_obj_set_size(scan_button, 110, kBackButtonHeight);
  lv_obj_align(scan_button, LV_ALIGN_TOP_RIGHT, -kSafeMargin, kSafeMargin + kStatusBarHeight + 8);
  lv_obj_set_style_bg_color(scan_button, lv_color_hex(kColorPanelLight), 0);
  lv_obj_set_style_border_color(scan_button, lv_color_hex(kColorAccent), 0);
  lv_obj_set_style_border_width(scan_button, 1, 0);
  lv_obj_set_style_shadow_width(scan_button, 0, 0);
  lv_obj_set_style_radius(scan_button, 8, 0);
  lv_obj_add_event_cb(
      scan_button, [](lv_event_t*) { runWifiScan(); }, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* scan_label = lv_label_create(scan_button);
  lv_label_set_text(scan_label, LV_SYMBOL_REFRESH " Scan");
  lv_obj_set_style_text_color(scan_label, lv_color_hex(kColorAccent), 0);
  lv_obj_center(scan_label);

  g_wifi_status = lv_label_create(screen);
  lv_label_set_text(g_wifi_status, "Tap Scan to discover nearby networks");
  lv_obj_set_style_text_color(g_wifi_status, lv_color_hex(kColorSecondary), 0);
  lv_obj_set_style_text_font(g_wifi_status, &lv_font_montserrat_16, 0);
  lv_obj_align(g_wifi_status, LV_ALIGN_TOP_LEFT, kSafeMargin, kContentTop);

  g_wifi_list = lv_list_create(screen);
  lv_obj_set_size(g_wifi_list, contentWidth(screen), contentHeight(screen) - 28);
  lv_obj_align(g_wifi_list, LV_ALIGN_TOP_MID, 0, kContentTop + 28);
  lv_obj_set_style_bg_color(g_wifi_list, lv_color_hex(kColorPanelLight), 0);
  lv_obj_set_style_radius(g_wifi_list, 10, 0);
  lv_obj_set_style_border_width(g_wifi_list, 0, 0);

  return screen;
}

lv_obj_t* buildEvidenceScreen(lv_obj_t* home) {
  using namespace reconclave::ui;
  lv_obj_t* screen = createScreen();
  addStatusBar(screen, "EVIDENCE", true);
  addBackButton(screen, home);

  constexpr lv_coord_t kRailWidth = 280;
  constexpr lv_coord_t kGap = 16;
  lv_obj_t* export_button = lv_button_create(screen);
  lv_obj_set_size(export_button, kRailWidth, 56);
  lv_obj_align(export_button, LV_ALIGN_TOP_LEFT, kSafeMargin, kContentTop);
  lv_obj_set_style_bg_color(export_button, lv_color_hex(kColorPanelLight), 0);
  lv_obj_set_style_border_color(export_button, lv_color_hex(kColorAmber), 0);
  lv_obj_set_style_border_width(export_button, 1, 0);
  lv_obj_set_style_shadow_width(export_button, 0, 0);
  lv_obj_set_style_radius(export_button, 10, 0);
  lv_obj_add_event_cb(
      export_button, [](lv_event_t*) { exportWifiEvidence(); }, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* export_label = lv_label_create(export_button);
  lv_label_set_text(export_label, LV_SYMBOL_SAVE " Export Wi-Fi scan");
  lv_obj_set_style_text_color(export_label, lv_color_hex(kColorAmber), 0);
  lv_obj_center(export_label);

  lv_obj_t* manifest_button = lv_button_create(screen);
  lv_obj_set_size(manifest_button, kRailWidth, 56);
  lv_obj_align_to(manifest_button, export_button, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 10);
  lv_obj_set_style_bg_color(manifest_button, lv_color_hex(kColorPanelLight), 0);
  lv_obj_set_style_border_color(manifest_button, lv_color_hex(kColorAccent), 0);
  lv_obj_set_style_border_width(manifest_button, 1, 0);
  lv_obj_set_style_shadow_width(manifest_button, 0, 0);
  lv_obj_set_style_radius(manifest_button, 10, 0);
  lv_obj_add_event_cb(
      manifest_button, [](lv_event_t*) { createEvidenceManifest(); }, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* manifest_label = lv_label_create(manifest_button);
  lv_label_set_text(manifest_label, LV_SYMBOL_OK " Verify + manifest");
  lv_obj_set_style_text_color(manifest_label, lv_color_hex(kColorAccent), 0);
  lv_obj_center(manifest_label);

  lv_obj_t* screenshot_button = lv_button_create(screen);
  lv_obj_set_size(screenshot_button, kRailWidth, 56);
  lv_obj_align_to(screenshot_button, manifest_button, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 10);
  lv_obj_set_style_bg_color(screenshot_button, lv_color_hex(kColorPanelLight), 0);
  lv_obj_set_style_border_color(screenshot_button, lv_color_hex(kColorAccent), 0);
  lv_obj_set_style_border_width(screenshot_button, 1, 0);
  lv_obj_set_style_shadow_width(screenshot_button, 0, 0);
  lv_obj_set_style_radius(screenshot_button, 10, 0);
  lv_obj_add_event_cb(screenshot_button, [](lv_event_t*) {
    startUiScreenshot(g_evidence_status);
  }, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* screenshot_label = lv_label_create(screenshot_button);
  lv_label_set_text(screenshot_label, LV_SYMBOL_IMAGE " Capture UI screenshot");
  lv_obj_set_style_text_color(screenshot_label, lv_color_hex(kColorAccent), 0);
  lv_obj_center(screenshot_label);

  lv_obj_t* summary_card = lv_obj_create(screen);
  lv_obj_set_size(summary_card, contentWidth(screen) - kRailWidth - kGap, contentHeight(screen));
  lv_obj_align(summary_card, LV_ALIGN_TOP_RIGHT, -kSafeMargin, kContentTop);
  lv_obj_set_style_bg_color(summary_card, lv_color_hex(kColorPanelLight), 0);
  lv_obj_set_style_border_width(summary_card, 0, 0);
  lv_obj_set_style_radius(summary_card, 12, 0);
  lv_obj_set_style_pad_all(summary_card, 16, 0);
  lv_obj_set_scroll_dir(summary_card, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(summary_card, LV_SCROLLBAR_MODE_AUTO);
  g_evidence_summary = lv_label_create(summary_card);
  lv_obj_set_width(g_evidence_summary, lv_pct(100));
  lv_obj_set_style_text_color(g_evidence_summary, lv_color_hex(kColorForeground), 0);
  lv_obj_set_style_text_font(g_evidence_summary, &lv_font_montserrat_16, 0);
  lv_obj_set_width(g_evidence_summary, lv_pct(100));
  lv_label_set_long_mode(g_evidence_summary, LV_LABEL_LONG_MODE_WRAP);
  refreshEvidenceSummary();

  g_evidence_status = lv_label_create(screen);
  lv_label_set_text(g_evidence_status, "READY  //  Select an evidence action");
  lv_obj_set_style_text_color(g_evidence_status, lv_color_hex(kColorSecondary), 0);
  lv_obj_set_style_text_font(g_evidence_status, &lv_font_montserrat_14, 0);
  lv_obj_set_width(g_evidence_status, kRailWidth);
  lv_label_set_long_mode(g_evidence_status, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_align(g_evidence_status, LV_ALIGN_TOP_LEFT, kSafeMargin, kContentTop + 208);

  return screen;
}

lv_obj_t* buildNetworkScreen(lv_obj_t* home) {
  using namespace reconclave::ui;
  lv_obj_t* screen = createScreen();
  addStatusBar(screen, "NETWORK", true);
  addBackButton(screen, home);

  lv_obj_t* refresh_button = lv_button_create(screen);
  lv_obj_set_size(refresh_button, 120, kBackButtonHeight);
  lv_obj_align(refresh_button, LV_ALIGN_TOP_RIGHT, -kSafeMargin, kSafeMargin + kStatusBarHeight + 8);
  lv_obj_set_style_bg_color(refresh_button, lv_color_hex(kColorPanelLight), 0);
  lv_obj_set_style_border_color(refresh_button, lv_color_hex(kColorAccent), 0);
  lv_obj_set_style_border_width(refresh_button, 1, 0);
  lv_obj_set_style_shadow_width(refresh_button, 0, 0);
  lv_obj_set_style_radius(refresh_button, 8, 0);
  lv_obj_add_event_cb(refresh_button, [](lv_event_t*) { refreshNetworkDetails(); }, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* refresh_label = lv_label_create(refresh_button);
  lv_label_set_text(refresh_label, LV_SYMBOL_REFRESH " Refresh");
  lv_obj_set_style_text_color(refresh_label, lv_color_hex(kColorAccent), 0);
  lv_obj_center(refresh_label);

  constexpr lv_coord_t kSummaryWidth = 360;
  constexpr lv_coord_t kGap = 16;
  lv_obj_t* summary_card = lv_obj_create(screen);
  lv_obj_set_size(summary_card, kSummaryWidth, contentHeight(screen));
  lv_obj_align(summary_card, LV_ALIGN_TOP_LEFT, kSafeMargin, kContentTop);
  lv_obj_set_style_bg_color(summary_card, lv_color_hex(kColorPanelLight), 0);
  lv_obj_set_style_border_color(summary_card, lv_color_hex(kColorAccent), 0);
  lv_obj_set_style_border_width(summary_card, 1, 0);
  lv_obj_set_style_radius(summary_card, 10, 0);
  lv_obj_set_style_pad_all(summary_card, 18, 0);
  lv_obj_clear_flag(summary_card, LV_OBJ_FLAG_SCROLLABLE);

  g_network_summary = lv_label_create(summary_card);
  lv_obj_set_width(g_network_summary, lv_pct(100));
  lv_label_set_long_mode(g_network_summary, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_style_text_color(g_network_summary, lv_color_hex(kColorForeground), 0);
  lv_obj_set_style_text_font(g_network_summary, &lv_font_montserrat_16, 0);
  lv_obj_align(g_network_summary, LV_ALIGN_TOP_LEFT, 0, 0);

  lv_obj_t* card = lv_obj_create(screen);
  lv_obj_set_size(card, contentWidth(screen) - kSummaryWidth - kGap, contentHeight(screen));
  lv_obj_align(card, LV_ALIGN_TOP_RIGHT, -kSafeMargin, kContentTop);
  lv_obj_set_style_bg_color(card, lv_color_hex(0x091922), 0);
  lv_obj_set_style_border_color(card, lv_color_hex(kColorAccent), 0);
  lv_obj_set_style_radius(card, 10, 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_pad_all(card, 18, 0);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

  g_network_details = lv_label_create(card);
  lv_obj_set_style_text_color(g_network_details, lv_color_hex(kColorForeground), 0);
  lv_obj_set_style_text_font(g_network_details, &lv_font_montserrat_16, 0);
  lv_obj_set_width(g_network_details, lv_pct(100));
  lv_obj_set_style_text_align(g_network_details, LV_TEXT_ALIGN_LEFT, 0);
  lv_obj_align(g_network_details, LV_ALIGN_TOP_LEFT, 0, 0);
  refreshNetworkDetails();

  return screen;
}

lv_obj_t* buildDevicesScreen(lv_obj_t* home) {
  using namespace reconclave::ui;
  lv_obj_t* screen = createScreen();
  addStatusBar(screen, "DEVICES", true);
  addBackButton(screen, home);

  lv_obj_t* refresh_button = lv_button_create(screen);
  lv_obj_set_size(refresh_button, 120, kBackButtonHeight);
  lv_obj_align(refresh_button, LV_ALIGN_TOP_RIGHT, -kSafeMargin, kSafeMargin + kStatusBarHeight + 8);
  lv_obj_set_style_bg_color(refresh_button, lv_color_hex(kColorPanelLight), 0);
  lv_obj_set_style_border_color(refresh_button, lv_color_hex(kColorAccent), 0);
  lv_obj_set_style_border_width(refresh_button, 1, 0);
  lv_obj_set_style_shadow_width(refresh_button, 0, 0);
  lv_obj_set_style_radius(refresh_button, 8, 0);
  lv_obj_add_event_cb(refresh_button, [](lv_event_t*) {
    if (g_device_details != nullptr) refreshDeviceDetails();
  }, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* refresh_label = lv_label_create(refresh_button);
  lv_label_set_text(refresh_label, LV_SYMBOL_REFRESH " Refresh");
  lv_obj_set_style_text_color(refresh_label, lv_color_hex(kColorAccent), 0);
  lv_obj_center(refresh_label);

  // Brightness moved to SETTINGS (Display & hardware section) - this
  // screen is device diagnostics only now, matching k230_phone_ui's own
  // separation between its "System"/"About phone" pages and its
  // "Display" settings page (see README's Settings section).
  constexpr lv_coord_t kGap = 16;
  const lv_coord_t panel_width = (contentWidth(screen) - kGap) / 2;
  lv_obj_t* card = lv_obj_create(screen);
  lv_obj_set_size(card, panel_width, contentHeight(screen));
  lv_obj_align(card, LV_ALIGN_TOP_LEFT, kSafeMargin, kContentTop);
  lv_obj_set_style_bg_color(card, lv_color_hex(kColorPanelLight), 0);
  lv_obj_set_style_radius(card, 10, 0);
  lv_obj_set_style_border_width(card, 0, 0);
  lv_obj_set_style_pad_all(card, 14, 0);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

  g_device_details = lv_label_create(card);
  lv_obj_set_width(g_device_details, lv_pct(100));
  lv_label_set_long_mode(g_device_details, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_style_text_color(g_device_details, lv_color_hex(kColorForeground), 0);
  lv_obj_set_style_text_font(g_device_details, &lv_font_montserrat_16, 0);
  lv_obj_align(g_device_details, LV_ALIGN_TOP_LEFT, 0, 0);

  lv_obj_t* services_card = lv_obj_create(screen);
  lv_obj_set_size(services_card, panel_width, contentHeight(screen));
  lv_obj_align(services_card, LV_ALIGN_TOP_RIGHT, -kSafeMargin, kContentTop);
  lv_obj_set_style_bg_color(services_card, lv_color_hex(0x091922), 0);
  lv_obj_set_style_border_color(services_card, lv_color_hex(kColorAccent), 0);
  lv_obj_set_style_border_width(services_card, 1, 0);
  lv_obj_set_style_radius(services_card, 10, 0);
  lv_obj_set_style_pad_all(services_card, 14, 0);
  lv_obj_clear_flag(services_card, LV_OBJ_FLAG_SCROLLABLE);

  g_device_services = lv_label_create(services_card);
  lv_obj_set_width(g_device_services, lv_pct(100));
  lv_label_set_long_mode(g_device_services, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_style_text_color(g_device_services, lv_color_hex(kColorForeground), 0);
  lv_obj_set_style_text_font(g_device_services, &lv_font_montserrat_16, 0);
  lv_obj_align(g_device_services, LV_ALIGN_TOP_LEFT, 0, 0);

  refreshDeviceDetails();

  return screen;
}

// Settings screen - grouped sections of navigable/inline rows, styled after
// k230_phone_ui's own Settings app (revived from the stock firmware backup
// and inspected directly on-device - see README's Settings section): bold
// section title + grey subtitle, then rows with an icon, title, description
// and either a chevron (navigates) or an inline control (adjusts in place).
// Unlike phone_ui's Settings, this doesn't duplicate the home tiles that
// are already full top-level tools (Recon, Wireless, Evidence, ...) - it
// covers what didn't otherwise have a home: hardware control (brightness,
// moved out of DEVICES) and trust/security status, plus short links to the
// two screens that are genuinely "configuration", not "a tool" (Network,
// Device info).
std::string trustStatusLine() {
  const std::string status = readTextFile("/run/reconclave/node-status");
  std::istringstream lines(status);
  std::string line;
  while (std::getline(lines, line)) {
    if (line.rfind("TRUST", 0) == 0) {
      // "TRUST             provisioned" -> "Execution trust: provisioned"
      const auto value_start = line.find_first_not_of(' ', 5);
      return "Execution trust: " +
             (value_start == std::string::npos ? "unknown" : line.substr(value_start));
    }
  }
  return "Execution trust: node service offline";
}

void addSettingsSection(lv_obj_t* parent, const char* title, const char* subtitle) {
  using namespace reconclave::ui;
  lv_obj_t* wrap = lv_obj_create(parent);
  lv_obj_set_size(wrap, lv_pct(100), 46);
  lv_obj_set_style_bg_opa(wrap, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(wrap, 0, 0);
  lv_obj_set_style_pad_all(wrap, 0, 0);
  lv_obj_clear_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* title_label = lv_label_create(wrap);
  lv_label_set_text(title_label, title);
  lv_obj_set_style_text_font(title_label, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(title_label, lv_color_hex(kColorForeground), 0);
  lv_obj_align(title_label, LV_ALIGN_TOP_LEFT, 0, 8);

  lv_obj_t* subtitle_label = lv_label_create(wrap);
  lv_label_set_text(subtitle_label, subtitle);
  lv_obj_set_style_text_font(subtitle_label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(subtitle_label, lv_color_hex(kColorSecondary), 0);
  lv_obj_align(subtitle_label, LV_ALIGN_TOP_LEFT, 0, 30);
}

// A navigable row (target != nullptr, shows a chevron) or a purely
// informational one (target == nullptr) - returns the row so a caller can
// still attach its own content (see the brightness/trust rows below).
lv_obj_t* addSettingsRow(lv_obj_t* parent, const char* symbol, const char* title,
                        const char* subtitle, lv_obj_t* target) {
  using namespace reconclave::ui;
  lv_obj_t* row = lv_obj_create(parent);
  lv_obj_set_size(row, lv_pct(100), 66);
  lv_obj_set_style_bg_color(row, lv_color_hex(kColorPanelLight), 0);
  lv_obj_set_style_bg_color(row, lv_color_hex(0x21485a), LV_STATE_PRESSED);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_set_style_radius(row, 10, 0);
  lv_obj_set_style_pad_all(row, 14, 0);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  if (target != nullptr) {
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, [](lv_event_t* event) {
      lv_screen_load(static_cast<lv_obj_t*>(lv_event_get_user_data(event)));
    }, LV_EVENT_CLICKED, target);
  }

  lv_obj_t* icon = lv_label_create(row);
  lv_label_set_text(icon, symbol);
  lv_obj_set_style_text_font(icon, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(icon, lv_color_hex(kColorAccent), 0);
  lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);

  lv_obj_t* title_label = lv_label_create(row);
  lv_label_set_text(title_label, title);
  lv_obj_set_width(title_label, target != nullptr ? lv_pct(84) : lv_pct(52));
  lv_label_set_long_mode(title_label, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_style_text_font(title_label, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(title_label, lv_color_hex(kColorForeground), 0);
  lv_obj_align(title_label, LV_ALIGN_TOP_LEFT, 44, -6);

  lv_obj_t* subtitle_label = lv_label_create(row);
  lv_label_set_text(subtitle_label, subtitle);
  lv_obj_set_width(subtitle_label, target != nullptr ? lv_pct(84) : lv_pct(52));
  lv_label_set_long_mode(subtitle_label, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_style_text_font(subtitle_label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(subtitle_label, lv_color_hex(kColorSecondary), 0);
  lv_obj_align(subtitle_label, LV_ALIGN_BOTTOM_LEFT, 44, 6);

  if (target != nullptr) {
    lv_obj_t* chevron = lv_label_create(row);
    lv_label_set_text(chevron, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(chevron, lv_color_hex(kColorSecondary), 0);
    lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, -8, 0);
  }
  return row;
}

void refreshWifiSetupList() {
  using namespace reconclave::ui;
  if (g_wifisetup_list == nullptr) return;
  lv_obj_clean(g_wifisetup_list);
  if (g_wifisetup_scan.empty()) {
    lv_obj_t* empty = lv_label_create(g_wifisetup_list);
    lv_label_set_text(empty, "Tap Scan to find nearby networks.");
    lv_obj_set_style_text_color(empty, lv_color_hex(kColorSecondary), 0);
    lv_obj_set_style_text_font(empty, &lv_font_montserrat_14, 0);
    return;
  }
  for (const auto& ap : g_wifisetup_scan) {
    const std::string name = ap.ssid.empty() ? "(hidden network)" : ap.ssid;
    lv_obj_t* row = lv_obj_create(g_wifisetup_list);
    lv_obj_set_size(row, lv_pct(100), 58);
    lv_obj_set_style_bg_color(row, lv_color_hex(kColorPanelLight), 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x21485a), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(row, lv_color_hex(ap.secured ? kColorAccent : kColorUnavailable), 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* icon = lv_label_create(row);
    lv_label_set_text(icon, ap.secured ? LV_SYMBOL_WIFI : LV_SYMBOL_WARNING);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(ap.secured ? kColorAccent : kColorSecondary), 0);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 12, 0);

    lv_obj_t* name_label = lv_label_create(row);
    lv_label_set_text(name_label, name.c_str());
    lv_obj_set_width(name_label, lv_pct(55));
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(name_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(name_label, lv_color_hex(kColorForeground), 0);
    lv_obj_align(name_label, LV_ALIGN_LEFT_MID, 38, 0);

    const std::string detail = std::to_string(ap.rssi_dbm) + " dBm  " +
        (ap.secured ? "secured" : "open - not supported yet");
    lv_obj_t* detail_label = lv_label_create(row);
    lv_label_set_text(detail_label, detail.c_str());
    lv_obj_set_style_text_font(detail_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(detail_label, lv_color_hex(kColorSecondary), 0);
    lv_obj_align(detail_label, LV_ALIGN_RIGHT_MID, -12, 0);

    // Only secured networks are tappable - reconclave-wifi always requires
    // an 8+ character password (it calls wpa_passphrase unconditionally),
    // so there's no working connect flow for open networks yet.
    if (ap.secured) {
      lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
      auto* ssid_copy = new std::string(ap.ssid);
      lv_obj_add_event_cb(row, [](lv_event_t* event) {
        auto* ssid_ptr = static_cast<std::string*>(lv_event_get_user_data(event));
        if (ssid_ptr != nullptr) showWifiPasswordScreen(*ssid_ptr);
      }, LV_EVENT_CLICKED, ssid_copy);
      lv_obj_add_event_cb(row, [](lv_event_t* event) {
        delete static_cast<std::string*>(lv_event_get_user_data(event));
      }, LV_EVENT_DELETE, ssid_copy);
    }
  }
}

void showWifiPasswordScreen(const std::string& ssid) {
  if (g_wifi_password_screen == nullptr) return;
  g_wifi_pending_ssid = ssid;
  lv_label_set_text(g_wifi_password_title, ("Connect to " + ssid).c_str());
  if (g_wifi_password_field != nullptr) lv_textarea_set_text(g_wifi_password_field, "");
  lv_label_set_text(g_wifi_password_status, "Enter the network password (min 8 characters).");
  lv_screen_load(g_wifi_password_screen);
}

lv_obj_t* buildWifiSetupScreen(lv_obj_t* home) {
  using namespace reconclave::ui;
  lv_obj_t* screen = createScreen();
  addStatusBar(screen, "WI-FI", true);
  addBackButton(screen, home);

  lv_obj_t* scan_button = lv_button_create(screen);
  lv_obj_set_size(scan_button, 110, kBackButtonHeight);
  lv_obj_align(scan_button, LV_ALIGN_TOP_RIGHT, -kSafeMargin, kSafeMargin + kStatusBarHeight + 8);
  lv_obj_set_style_bg_color(scan_button, lv_color_hex(kColorPanelLight), 0);
  lv_obj_set_style_border_color(scan_button, lv_color_hex(kColorAccent), 0);
  lv_obj_set_style_border_width(scan_button, 1, 0);
  lv_obj_set_style_shadow_width(scan_button, 0, 0);
  lv_obj_set_style_radius(scan_button, 8, 0);
  lv_obj_add_event_cb(scan_button, [](lv_event_t*) {
    std::string error;
    reconclave::scanWifi("wlan0", g_wifisetup_scan, error);
    std::sort(g_wifisetup_scan.begin(), g_wifisetup_scan.end(),
             [](const auto& left, const auto& right) { return left.rssi_dbm > right.rssi_dbm; });
    refreshWifiSetupList();
  }, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* scan_label = lv_label_create(scan_button);
  lv_label_set_text(scan_label, LV_SYMBOL_REFRESH " Scan");
  lv_obj_set_style_text_color(scan_label, lv_color_hex(kColorAccent), 0);
  lv_obj_center(scan_label);

  lv_obj_t* disconnect_button = lv_button_create(screen);
  lv_obj_set_size(disconnect_button, 150, kBackButtonHeight);
  lv_obj_align(disconnect_button, LV_ALIGN_TOP_RIGHT, -kSafeMargin - 110 - 10,
               kSafeMargin + kStatusBarHeight + 8);
  lv_obj_set_style_bg_color(disconnect_button, lv_color_hex(kColorPanelLight), 0);
  lv_obj_set_style_border_color(disconnect_button, lv_color_hex(kColorAmber), 0);
  lv_obj_set_style_border_width(disconnect_button, 1, 0);
  lv_obj_set_style_shadow_width(disconnect_button, 0, 0);
  lv_obj_set_style_radius(disconnect_button, 8, 0);
  lv_obj_add_event_cb(disconnect_button, [](lv_event_t*) {
    if (g_wifi_op_running.load()) return;
    lv_label_set_text(g_wifisetup_status, "Disconnecting...");
    lv_refr_now(nullptr);
    startWifiDisconnect();
  }, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* disconnect_label = lv_label_create(disconnect_button);
  lv_label_set_text(disconnect_label, LV_SYMBOL_CLOSE " Disconnect");
  lv_obj_set_style_text_color(disconnect_label, lv_color_hex(kColorAmber), 0);
  lv_obj_center(disconnect_label);

  g_wifisetup_status = lv_label_create(screen);
  lv_label_set_text(g_wifisetup_status, "Checking current connection...");
  lv_obj_set_width(g_wifisetup_status, contentWidth(screen));
  lv_label_set_long_mode(g_wifisetup_status, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_style_text_color(g_wifisetup_status, lv_color_hex(kColorSecondary), 0);
  lv_obj_set_style_text_font(g_wifisetup_status, &lv_font_montserrat_14, 0);
  lv_obj_align(g_wifisetup_status, LV_ALIGN_TOP_LEFT, kSafeMargin, kContentTop);

  g_wifisetup_list = lv_list_create(screen);
  lv_obj_set_size(g_wifisetup_list, contentWidth(screen), contentHeight(screen) - 28);
  lv_obj_align(g_wifisetup_list, LV_ALIGN_TOP_MID, 0, kContentTop + 28);
  lv_obj_set_style_bg_color(g_wifisetup_list, lv_color_hex(kColorPanelLight), 0);
  lv_obj_set_style_radius(g_wifisetup_list, 10, 0);
  lv_obj_set_style_border_width(g_wifisetup_list, 0, 0);
  refreshWifiSetupList();

  // Query current status fresh each time this screen is entered (cheap,
  // bounded one-shot - see currentWifiLinkStatus()), not on a repeating
  // timer - there's no continuous background polling here, unlike the
  // camera preview, since a manual Wi-Fi settings screen doesn't need
  // that and it would mean forking `iwconfig` every few seconds forever.
  lv_obj_add_event_cb(screen, [](lv_event_t*) {
    const auto link = currentWifiLinkStatus();
    if (g_wifisetup_status == nullptr) return;
    lv_label_set_text(g_wifisetup_status, link.associated
        ? ("Connected to " + link.ssid + (link.ip.empty() ? "" : ("  |  " + link.ip))).c_str()
        : "Not connected.");
  }, LV_EVENT_SCREEN_LOADED, nullptr);

  g_wifisetup_screen = screen;
  return screen;
}

lv_obj_t* buildWifiPasswordScreen(lv_obj_t* home) {
  using namespace reconclave::ui;
  lv_obj_t* screen = createScreen();
  addStatusBar(screen, "WI-FI PASSWORD", true);
  // Only ever opened from the Wi-Fi setup screen's network list.
  addDynamicBackButton(screen, &g_wifisetup_screen, home);

  g_wifi_password_title = lv_label_create(screen);
  lv_label_set_text(g_wifi_password_title, "Connect to network");
  lv_obj_set_width(g_wifi_password_title, contentWidth(screen));
  lv_label_set_long_mode(g_wifi_password_title, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_style_text_color(g_wifi_password_title, lv_color_hex(kColorForeground), 0);
  lv_obj_set_style_text_font(g_wifi_password_title, &lv_font_montserrat_22, 0);
  lv_obj_align(g_wifi_password_title, LV_ALIGN_TOP_LEFT, kSafeMargin, kContentTop);

  lv_obj_t* keyboard = lv_keyboard_create(screen);
  lv_obj_set_size(keyboard, contentWidth(screen), 218);
  lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, -kSafeMargin);
  lv_obj_set_style_bg_color(keyboard, lv_color_hex(kColorPanel), 0);

  g_wifi_password_field = lv_textarea_create(screen);
  lv_obj_set_size(g_wifi_password_field, contentWidth(screen) - 176, 48);
  lv_obj_align(g_wifi_password_field, LV_ALIGN_TOP_LEFT, kSafeMargin, kContentTop + 40);
  lv_textarea_set_one_line(g_wifi_password_field, true);
  lv_textarea_set_password_mode(g_wifi_password_field, true);
  lv_textarea_set_max_length(g_wifi_password_field, 63);
  lv_textarea_set_placeholder_text(g_wifi_password_field, "Password");
  lv_obj_set_style_bg_color(g_wifi_password_field, lv_color_hex(kColorPanelLight), 0);
  lv_obj_set_style_text_color(g_wifi_password_field, lv_color_hex(kColorForeground), 0);
  lv_obj_set_style_border_color(g_wifi_password_field, lv_color_hex(kColorAccent), LV_STATE_FOCUSED);
  lv_keyboard_set_textarea(keyboard, g_wifi_password_field);
  lv_obj_add_event_cb(g_wifi_password_field, [](lv_event_t* event) {
    lv_keyboard_set_textarea(static_cast<lv_obj_t*>(lv_event_get_user_data(event)), g_wifi_password_field);
  }, LV_EVENT_FOCUSED, keyboard);

  g_wifi_password_connect_button = lv_button_create(screen);
  lv_obj_set_size(g_wifi_password_connect_button, 160, 48);
  lv_obj_align(g_wifi_password_connect_button, LV_ALIGN_TOP_RIGHT, -kSafeMargin, kContentTop + 40);
  lv_obj_set_style_bg_color(g_wifi_password_connect_button, lv_color_hex(kColorAccent), 0);
  lv_obj_set_style_radius(g_wifi_password_connect_button, 8, 0);
  lv_obj_set_style_shadow_width(g_wifi_password_connect_button, 0, 0);
  lv_obj_add_event_cb(g_wifi_password_connect_button, [](lv_event_t*) {
    if (g_wifi_op_running.load()) return;
    const std::string password = lv_textarea_get_text(g_wifi_password_field);
    if (password.size() < 8) {
      lv_label_set_text(g_wifi_password_status, "Password must be at least 8 characters.");
      return;
    }
    lv_label_set_text(g_wifi_password_status, ("Connecting to " + g_wifi_pending_ssid + "...").c_str());
    lv_refr_now(nullptr);
    startWifiConnect(g_wifi_pending_ssid, password);
  }, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* connect_label = lv_label_create(g_wifi_password_connect_button);
  lv_label_set_text(connect_label, LV_SYMBOL_OK " Connect");
  lv_obj_set_style_text_color(connect_label, lv_color_hex(kColorPanel), 0);
  lv_obj_center(connect_label);

  g_wifi_password_status = lv_label_create(screen);
  lv_label_set_text(g_wifi_password_status, "Enter the network password (min 8 characters).");
  lv_obj_set_width(g_wifi_password_status, contentWidth(screen));
  lv_label_set_long_mode(g_wifi_password_status, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_style_text_color(g_wifi_password_status, lv_color_hex(kColorSecondary), 0);
  lv_obj_set_style_text_font(g_wifi_password_status, &lv_font_montserrat_14, 0);
  lv_obj_align(g_wifi_password_status, LV_ALIGN_TOP_LEFT, kSafeMargin, kContentTop + 100);

  g_wifi_password_screen = screen;
  return screen;
}

lv_obj_t* buildSettingsScreen(lv_obj_t* home, lv_obj_t* network_screen, lv_obj_t* wifi_setup_screen,
                              lv_obj_t* devices_screen, lv_obj_t* node_screen,
                              lv_obj_t* location_screen) {
  using namespace reconclave::ui;
  lv_obj_t* screen = createScreen();
  addStatusBar(screen, "SETTINGS", true);
  addBackButton(screen, home);

  lv_obj_t* list = lv_obj_create(screen);
  lv_obj_set_size(list, contentWidth(screen), contentHeight(screen));
  lv_obj_align(list, LV_ALIGN_TOP_MID, 0, kContentTop);
  lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(list, 0, 0);
  lv_obj_set_style_pad_all(list, 0, 0);
  lv_obj_set_style_pad_row(list, 10, 0);
  lv_obj_set_style_width(list, 5, LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_color(list, lv_color_hex(kColorAccent), LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_opa(list, LV_OPA_40, LV_PART_SCROLLBAR);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

  addSettingsSection(list, "Connections", "Network, wireless and location");
  addSettingsRow(list, LV_SYMBOL_GPS, "Network", "Interface, route and connectivity", network_screen);
  // Wi-Fi here means *joining* a network (like a phone's Wi-Fi settings),
  // not the WIRELESS home tile's survey/recon tool - a real, separate
  // screen (buildWifiSetupScreen), not a link to that tool. See README's
  // Settings section for why those two are deliberately not the same
  // screen.
  addSettingsRow(list, LV_SYMBOL_WIFI, "Wi-Fi", "Connect to a wireless network", wifi_setup_screen);
  lv_obj_t* bluetooth_row = addSettingsRow(list, LV_SYMBOL_BLUETOOTH, "Bluetooth",
                                           "Radio unavailable in this hardware build", nullptr);
  lv_obj_t* bluetooth_status = lv_label_create(bluetooth_row);
  lv_label_set_text(bluetooth_status, "UNAVAILABLE");
  lv_obj_set_width(bluetooth_status, lv_pct(42));
  lv_label_set_long_mode(bluetooth_status, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_style_text_align(bluetooth_status, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_style_text_font(bluetooth_status, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(bluetooth_status, lv_color_hex(kColorSecondary), 0);
  lv_obj_align(bluetooth_status, LV_ALIGN_RIGHT_MID, 0, 0);
  addSettingsRow(list, LV_SYMBOL_GPS, "Location", "GNSS unavailable - modem firmware required",
                 location_screen);

  addSettingsSection(list, "Display & hardware", "Backlight");
  lv_obj_t* brightness_row = addSettingsRow(list, LV_SYMBOL_SETTINGS, "Brightness",
                                            "Drag to adjust the panel backlight", nullptr);
  lv_obj_set_height(brightness_row, 84);
  lv_obj_t* slider = lv_slider_create(brightness_row);
  lv_obj_set_size(slider, lv_pct(100), 14);
  lv_obj_align(slider, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_slider_set_range(slider, 5, 100);
  int percent = 75;
  if (findBacklight()) {
    percent = std::max(5, std::atoi(readFirstLine(g_backlight_path).c_str()) * 100 / g_backlight_max);
  }
  lv_slider_set_value(slider, percent, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(slider, lv_color_hex(kColorAccent), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(slider, lv_color_hex(kColorAmber), LV_PART_KNOB);
  lv_obj_set_style_pad_all(slider, 3, LV_PART_KNOB);
  lv_obj_add_event_cb(slider, [](lv_event_t* event) {
    if (g_backlight_path.empty() && !findBacklight()) return;
    const auto* source = static_cast<lv_obj_t*>(lv_event_get_target(event));
    const int raw = std::max(1, lv_slider_get_value(source) * g_backlight_max / 100);
    std::ofstream output(g_backlight_path);
    if (output) output << raw << "\n";
  }, LV_EVENT_VALUE_CHANGED, nullptr);

  lv_obj_t* screenshot_row = addSettingsRow(list, LV_SYMBOL_IMAGE, "Screenshot",
                                             "Save the active screen as a lossless PNG", nullptr);
  lv_obj_add_flag(screenshot_row, LV_OBJ_FLAG_CLICKABLE);
  g_screenshot_status = lv_label_create(screenshot_row);
  lv_label_set_text(g_screenshot_status, "TAP TO CAPTURE");
  lv_obj_set_width(g_screenshot_status, lv_pct(40));
  lv_label_set_long_mode(g_screenshot_status, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_style_text_align(g_screenshot_status, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_style_text_font(g_screenshot_status, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(g_screenshot_status, lv_color_hex(kColorAccent), 0);
  lv_obj_align(g_screenshot_status, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_add_event_cb(screenshot_row, [](lv_event_t*) {
    startUiScreenshot(g_screenshot_status);
  }, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* battery_row = addSettingsRow(list, LV_SYMBOL_BATTERY_FULL, "Battery",
                                         "Keyboard-base fuel gauge", nullptr);
  lv_obj_set_height(battery_row, 92);
  g_battery_details = lv_label_create(battery_row);
  lv_label_set_text(g_battery_details, batteryDetailsText().c_str());
  lv_obj_set_width(g_battery_details, 560);
  lv_label_set_long_mode(g_battery_details, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_style_text_align(g_battery_details, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_style_text_font(g_battery_details, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(g_battery_details, lv_color_hex(kColorForeground), 0);
  lv_obj_align(g_battery_details, LV_ALIGN_RIGHT_MID, 0, 0);

  addSettingsSection(list, "Trust & security", "Signed remote capabilities");
  lv_obj_t* trust_row = addSettingsRow(list, LV_SYMBOL_WARNING, "Execution trust",
                                       "Set on-device via SSH: reconclave-trust provision", nullptr);
  g_settings_trust_status = lv_label_create(trust_row);
  lv_label_set_text(g_settings_trust_status, trustStatusLine().c_str());
  lv_obj_set_width(g_settings_trust_status, lv_pct(42));
  lv_label_set_long_mode(g_settings_trust_status, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_style_text_align(g_settings_trust_status, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_style_text_font(g_settings_trust_status, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(g_settings_trust_status, lv_color_hex(kColorAccent), 0);
  lv_obj_align(g_settings_trust_status, LV_ALIGN_RIGHT_MID, 0, 0);

  addSettingsSection(list, "System & about", "Device information and jobs");
  addSettingsRow(list, LV_SYMBOL_SETTINGS, "Device info", "Firmware, kernel, storage and SSH",
                 devices_screen);
  addSettingsRow(list, LV_SYMBOL_LOOP, "Node & jobs", "Trust status, coordination and jobs",
                 node_screen);

  return screen;
}

void startPacketCapture(const char* preset) {
  if (g_capture_pid > 0) {
    lv_label_set_text(g_capture_status, LV_SYMBOL_WARNING " A capture is already running");
    return;
  }
  const NetworkState network = networkState();
  if (!network.connected || network.iface.empty()) {
    lv_label_set_text(g_capture_status, LV_SYMBOL_CLOSE " No active interface");
    return;
  }
  const pid_t child = fork();
  if (child < 0) {
    lv_label_set_text(g_capture_status, LV_SYMBOL_CLOSE " Unable to start capture");
    return;
  }
  if (child == 0) {
    const int log = open("/tmp/reconclave-capture.log", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (log >= 0) { dup2(log, STDOUT_FILENO); dup2(log, STDERR_FILENO); close(log); }
    execlp("reconclave-capture", "reconclave-capture", network.iface.c_str(), "30", preset,
           static_cast<char*>(nullptr));
    _exit(127);
  }
  g_capture_pid = child;
  if (g_capture_stop_button != nullptr) lv_obj_clear_state(g_capture_stop_button, LV_STATE_DISABLED);
  g_capture_preset = preset;
  const std::string message = std::string(LV_SYMBOL_REFRESH) + " Capturing " + preset + " on " +
                              network.iface + "\n30 seconds  |  evidence-safe PCAP";
  lv_label_set_text(g_capture_status, message.c_str());
}

lv_obj_t* buildCaptureScreen(lv_obj_t* home) {
  using namespace reconclave::ui;
  lv_obj_t* screen = createScreen();
  addStatusBar(screen, "PACKET CAPTURE", true);
  addBackButton(screen, home);

  lv_obj_t* stop = lv_button_create(screen);
  g_capture_stop_button = stop;
  lv_obj_set_size(stop, 120, kBackButtonHeight);
  lv_obj_align(stop, LV_ALIGN_TOP_RIGHT, -kSafeMargin, kSafeMargin + kStatusBarHeight + 8);
  lv_obj_set_style_bg_color(stop, lv_color_hex(kColorPanelLight), 0);
  lv_obj_set_style_bg_color(stop, lv_color_hex(kColorUnavailable), LV_STATE_DISABLED);
  lv_obj_set_style_border_color(stop, lv_color_hex(kColorAmber), 0);
  lv_obj_set_style_border_width(stop, 1, 0);
  lv_obj_set_style_radius(stop, 8, 0);
  lv_obj_set_style_shadow_width(stop, 0, 0);
  lv_obj_add_state(stop, LV_STATE_DISABLED);
  lv_obj_add_event_cb(stop, [](lv_event_t*) {
    if (g_capture_pid > 0) {
      kill(g_capture_pid, SIGTERM);
      lv_label_set_text(g_capture_status, LV_SYMBOL_STOP " Stopping capture...");
    }
  }, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* stop_label = lv_label_create(stop);
  lv_label_set_text(stop_label, LV_SYMBOL_STOP " Stop");
  lv_obj_set_style_text_color(stop_label, lv_color_hex(kColorAmber), 0);
  lv_obj_center(stop_label);

  lv_obj_t* intro = lv_label_create(screen);
  lv_label_set_text(intro, "SELECT TRAFFIC PROFILE");
  lv_obj_set_style_text_font(intro, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(intro, lv_color_hex(kColorAccent), 0);
  lv_obj_align(intro, LV_ALIGN_TOP_LEFT, kSafeMargin, kContentTop);

  struct Preset { const char* name; const char* detail; const char* preset; const char* symbol; };
  static const Preset presets[] = {
      {"ALL TRAFFIC", "Full packet metadata", "all", LV_SYMBOL_EYE_OPEN},
      {"DNS", "Name-resolution activity", "dns", LV_SYMBOL_EYE_OPEN},
      {"ARP", "Layer-2 neighbours", "arp", LV_SYMBOL_REFRESH},
      {"DHCP", "Address assignments", "dhcp", LV_SYMBOL_WIFI},
  };
  const lv_coord_t card_w = (contentWidth(screen) - 3 * 12) / 4;
  for (int i = 0; i < 4; ++i) {
    lv_obj_t* card = lv_button_create(screen);
    lv_obj_set_size(card, card_w, 126);
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, kSafeMargin + i * (card_w + 12), kContentTop + 28);
    lv_obj_set_style_bg_color(card, lv_color_hex(kColorPanelLight), 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x21485a), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(card, lv_color_hex(i == 0 ? kColorAmber : kColorAccent), 0);
    lv_obj_set_style_border_width(card, i == 0 ? 2 : 1, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    // The vendor button theme installs a flex layout. Without explicitly
    // disabling it, all three child labels are packed into the centre and
    // render on top of one another on the K230 panel.
    lv_obj_set_layout(card, LV_LAYOUT_NONE);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_add_event_cb(card, [](lv_event_t* event) {
      startPacketCapture(static_cast<const char*>(lv_event_get_user_data(event)));
    }, LV_EVENT_CLICKED, const_cast<char*>(presets[i].preset));
    lv_obj_t* icon = lv_label_create(card);
    lv_label_set_text(icon, presets[i].symbol);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(i == 0 ? kColorAmber : kColorAccent), 0);
    lv_obj_align(icon, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t* title = lv_label_create(card);
    lv_label_set_text(title, presets[i].name);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(kColorForeground), 0);
    lv_obj_set_width(title, card_w - 66);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 42, 3);
    lv_obj_t* detail = lv_label_create(card);
    lv_label_set_text(detail, presets[i].detail);
    lv_obj_set_width(detail, card_w - 28);
    lv_label_set_long_mode(detail, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(detail, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(detail, lv_color_hex(kColorSecondary), 0);
    lv_obj_align(detail, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  }

  lv_obj_t* status_card = lv_obj_create(screen);
  constexpr lv_coord_t kProfilesHeight = 126;
  constexpr lv_coord_t kConsoleGap = 16;
  const lv_coord_t console_height = contentHeight(screen) - 28 - kProfilesHeight - kConsoleGap;
  lv_obj_set_size(status_card, contentWidth(screen), console_height);
  lv_obj_align(status_card, LV_ALIGN_BOTTOM_MID, 0, -kSafeMargin);
  lv_obj_set_style_bg_color(status_card, lv_color_hex(0x091922), 0);
  lv_obj_set_style_border_color(status_card, lv_color_hex(kColorAccent), 0);
  lv_obj_set_style_border_width(status_card, 1, 0);
  lv_obj_set_style_radius(status_card, 10, 0);
  lv_obj_set_style_pad_all(status_card, 18, 0);
  lv_obj_clear_flag(status_card, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* status_heading = lv_label_create(status_card);
  lv_label_set_text(status_heading, "CAPTURE CONSOLE  //  LIVE STATUS & EVIDENCE OUTPUT");
  lv_obj_set_style_text_font(status_heading, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(status_heading, lv_color_hex(kColorAccent), 0);
  lv_obj_align(status_heading, LV_ALIGN_TOP_LEFT, 0, 0);

  g_capture_status = lv_label_create(status_card);
  lv_label_set_text(g_capture_status,
      "READY\n\nSelect a traffic profile to record 30 seconds on the active interface.\n"
      "The resulting PCAP and metadata are added to the evidence timeline.");
  lv_obj_set_width(g_capture_status, lv_pct(100));
  lv_label_set_long_mode(g_capture_status, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_style_text_font(g_capture_status, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(g_capture_status, lv_color_hex(kColorForeground), 0);
  lv_obj_align(g_capture_status, LV_ALIGN_TOP_LEFT, 0, 38);
  return screen;
}

void startNmap(const char* preset) {
  if (g_nmap_pid > 0) { lv_label_set_text(g_nmap_output, "SCAN ALREADY ACTIVE"); return; }
  const std::string target = attachedIpv4Network();
  if (target.empty()) { lv_label_set_text(g_nmap_output, "NO ATTACHED IPV4 TARGET"); return; }
  const pid_t child = fork();
  if (child < 0) { lv_label_set_text(g_nmap_output, "UNABLE TO START NMAP"); return; }
  if (child == 0) {
    const int log = open("/tmp/reconclave-nmap.log", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (log >= 0) { dup2(log, STDOUT_FILENO); dup2(log, STDERR_FILENO); close(log); }
    execlp("reconclave-nmap", "reconclave-nmap", target.c_str(), preset, static_cast<char*>(nullptr));
    _exit(127);
  }
  g_nmap_pid = child;
  lv_label_set_text(g_nmap_output,
      (std::string("RUNNING // ") + preset + " // " + target + "\nOutput will appear when complete...").c_str());
}

lv_obj_t* buildNmapScreen(lv_obj_t* home) {
  using namespace reconclave::ui;
  lv_obj_t* screen = createScreen();
  addStatusBar(screen, "NMAP", true);
  addBackButton(screen, home);
  struct Choice { const char* title; const char* preset; const char* symbol; };
  static const Choice choices[] = {
      {"HOST DISCOVERY", "discovery", LV_SYMBOL_REFRESH},
      {"QUICK PORTS", "quick", LV_SYMBOL_EYE_OPEN},
      {"SERVICE ID", "services", LV_SYMBOL_EYE_OPEN},
  };
  for (int i = 0; i < 3; ++i) {
    lv_obj_t* button = lv_button_create(screen);
    lv_obj_set_size(button, 270, kBackButtonHeight);
    lv_obj_align(button, LV_ALIGN_TOP_LEFT, kSafeMargin + 100 + i * 282,
                 kSafeMargin + kStatusBarHeight + 8);
    lv_obj_set_style_bg_color(button, lv_color_hex(kColorPanelLight), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x245267), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(button, lv_color_hex(i == 0 ? kColorAmber : kColorAccent), 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_radius(button, 8, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_add_event_cb(button, [](lv_event_t* event) {
      startNmap(static_cast<const char*>(lv_event_get_user_data(event)));
    }, LV_EVENT_CLICKED, const_cast<char*>(choices[i].preset));
    lv_obj_t* label = lv_label_create(button);
    const std::string action = std::string(choices[i].symbol) + "  " + choices[i].title;
    lv_label_set_text(label, action.c_str());
    lv_obj_set_width(label, lv_pct(94));
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(kColorForeground), 0);
    lv_obj_center(label);
  }
  lv_obj_t* pane = lv_obj_create(screen);
  lv_obj_set_size(pane, contentWidth(screen), contentHeight(screen));
  lv_obj_align(pane, LV_ALIGN_TOP_LEFT, kSafeMargin, kContentTop);
  lv_obj_set_style_bg_color(pane, lv_color_hex(0x050e13), 0);
  lv_obj_set_style_border_color(pane, lv_color_hex(kColorAccent), 0);
  lv_obj_set_style_border_width(pane, 1, 0);
  lv_obj_set_style_radius(pane, 8, 0);
  lv_obj_set_scroll_dir(pane, LV_DIR_VER);
  lv_obj_set_style_pad_all(pane, 16, 0);

  lv_obj_t* output_heading = lv_label_create(pane);
  lv_label_set_text(output_heading, "SCAN CONSOLE  //  RESULTS ARE SAVED TO EVIDENCE");
  lv_obj_set_style_text_font(output_heading, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(output_heading, lv_color_hex(kColorAccent), 0);
  lv_obj_align(output_heading, LV_ALIGN_TOP_LEFT, 0, 0);

  g_nmap_scope = lv_label_create(pane);
  lv_obj_set_width(g_nmap_scope, lv_pct(42));
  lv_label_set_long_mode(g_nmap_scope, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_style_text_align(g_nmap_scope, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_style_text_font(g_nmap_scope, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(g_nmap_scope, lv_color_hex(kColorAmber), 0);
  lv_obj_align(g_nmap_scope, LV_ALIGN_TOP_RIGHT, 0, 0);
  lv_obj_add_event_cb(screen, [](lv_event_t*) {
    const std::string target = attachedIpv4Network();
    const std::string scope = "SCOPE  //  " + (target.empty() ? std::string("NO ATTACHED IPV4") : target);
    lv_label_set_text(g_nmap_scope, scope.c_str());
  }, LV_EVENT_SCREEN_LOADED, nullptr);

  g_nmap_output = lv_label_create(pane);
  lv_label_set_text(g_nmap_output, "READY\n\nSelect a scan preset above to begin.");
  lv_obj_set_width(g_nmap_output, lv_pct(100));
  lv_label_set_long_mode(g_nmap_output, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_style_text_font(g_nmap_output, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(g_nmap_output, lv_color_hex(kColorForeground), 0);
  lv_obj_align(g_nmap_output, LV_ALIGN_TOP_LEFT, 0, 34);
  return screen;
}

lv_obj_t* createHomeAction(lv_obj_t* parent, int index, const char* title, const char* detail,
                           const char* symbol, const char* badge, std::uint32_t color,
                           lv_obj_t* target, bool scan_wifi = false) {
  using namespace reconclave::ui;
  lv_obj_t* card = lv_button_create(parent);
  lv_obj_set_style_bg_color(card, lv_color_hex(kColorPanelLight), 0);
  lv_obj_set_style_bg_color(card, lv_color_hex(0x245267), LV_STATE_PRESSED);
  lv_obj_set_style_border_color(card, lv_color_hex(color), 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_opa(card, LV_OPA_60, 0);
  lv_obj_set_style_radius(card, 10, 0);
  lv_obj_set_style_shadow_width(card, 8, 0);
  lv_obj_set_style_shadow_opa(card, LV_OPA_20, 0);
  lv_obj_set_style_pad_all(card, 12, 0);

  char number[8]; std::snprintf(number, sizeof(number), "%02d", index);
  lv_obj_t* number_label = lv_label_create(card);
  lv_label_set_text(number_label, number);
  lv_obj_set_style_text_font(number_label, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(number_label, lv_color_hex(kColorSecondary), 0);
  lv_obj_align(number_label, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_t* icon = lv_label_create(card);
  lv_label_set_text(icon, symbol);
  lv_obj_set_style_text_font(icon, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(icon, lv_color_hex(color), 0);
  lv_obj_align(icon, LV_ALIGN_TOP_RIGHT, 0, 0);
  lv_obj_t* title_label = lv_label_create(card);
  lv_label_set_text(title_label, title);
  lv_obj_set_style_text_font(title_label, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(title_label, lv_color_hex(kColorForeground), 0);
  lv_obj_align(title_label, LV_ALIGN_LEFT_MID, 0, -8);
  lv_obj_t* detail_label = lv_label_create(card);
  lv_label_set_text(detail_label, detail);
  // Reserve the right-hand quarter for the action badge. Several useful
  // descriptions are long enough to collide with it at 88% card width.
  lv_obj_set_width(detail_label, lv_pct(72));
  lv_label_set_long_mode(detail_label, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_style_text_font(detail_label, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(detail_label, lv_color_hex(kColorSecondary), 0);
  lv_obj_align(detail_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_obj_t* badge_label = lv_label_create(card);
  lv_label_set_text(badge_label, badge);
  lv_obj_set_style_text_font(badge_label, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(badge_label, lv_color_hex(color), 0);
  lv_obj_align(badge_label, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  struct Launch { lv_obj_t* target; bool scan; };
  auto* launch = new Launch{target, scan_wifi};
  lv_obj_add_event_cb(card, [](lv_event_t* event) {
    auto* launch = static_cast<Launch*>(lv_event_get_user_data(event));
    lv_screen_load(launch->target);
    if (launch->scan) runWifiScan();
  }, LV_EVENT_CLICKED, launch);
  lv_obj_add_event_cb(card, [](lv_event_t* event) {
    delete static_cast<Launch*>(lv_event_get_user_data(event));
  }, LV_EVENT_DELETE, launch);
  return card;
}

lv_obj_t* createHomeUtility(lv_obj_t* parent, const char* symbol, const char* title, lv_obj_t* target) {
  using namespace reconclave::ui;
  lv_obj_t* button = lv_button_create(parent);
  lv_obj_set_style_bg_color(button, lv_color_hex(0x0a1a23), 0);
  lv_obj_set_style_bg_color(button, lv_color_hex(kColorPanelLight), LV_STATE_PRESSED);
  lv_obj_set_style_border_width(button, 0, 0);
  lv_obj_set_style_radius(button, 7, 0);
  lv_obj_set_style_shadow_width(button, 0, 0);
  lv_obj_add_event_cb(button, [](lv_event_t* event) {
    lv_screen_load(static_cast<lv_obj_t*>(lv_event_get_user_data(event)));
  }, LV_EVENT_CLICKED, target);
  lv_obj_t* label = lv_label_create(button);
  const std::string text = std::string(symbol) + "  " + title;
  lv_label_set_text(label, text.c_str());
  lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(label, lv_color_hex(kColorForeground), 0);
  lv_obj_center(label);
  return button;
}

lv_obj_t* buildToolsScreen(lv_obj_t* home, lv_obj_t* recon_screen, lv_obj_t* nmap_screen,
                           lv_obj_t* wireless_screen, lv_obj_t* capture_screen,
                           lv_obj_t* findings_screen, lv_obj_t* evidence_screen,
                           lv_obj_t* vision_screen, lv_obj_t* location_screen) {
  using namespace reconclave::ui;
  lv_obj_t* screen = createScreen();
  addStatusBar(screen, "TOOLS", true);
  addBackButton(screen, home);

  lv_obj_t* heading = lv_label_create(screen);
  lv_label_set_text(heading, "FIELD TOOLKIT  //  DISCOVER  •  INSPECT  •  RECORD");
  lv_obj_set_style_text_font(heading, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(heading, lv_color_hex(kColorAccent), 0);
  lv_obj_align(heading, LV_ALIGN_TOP_LEFT, kSafeMargin + 110,
               kSafeMargin + kStatusBarHeight + 19);

  lv_obj_t* grid = lv_obj_create(screen);
  lv_obj_set_size(grid, contentWidth(screen), contentHeight(screen));
  lv_obj_align(grid, LV_ALIGN_TOP_LEFT, kSafeMargin, kContentTop);
  lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(grid, 0, 0);
  lv_obj_set_style_pad_all(grid, 0, 0);
  lv_obj_set_style_pad_gap(grid, 10, 0);
  lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

  struct ToolSpec {
    const char* title;
    const char* detail;
    const char* symbol;
    const char* badge;
    std::uint32_t color;
    lv_obj_t* target;
    bool scan;
  };
  const ToolSpec tools[] = {
      {"LAN SURVEY", "Hosts, ports and service posture", LV_SYMBOL_REFRESH, "DISCOVER", kColorAccent, recon_screen, false},
      {"NMAP", "Focused host and service scans", LV_SYMBOL_EYE_OPEN, "SCAN", kColorAmber, nmap_screen, false},
      {"WI-FI SURVEY", "Access points, channels and security", LV_SYMBOL_WIFI, "RF", kColorAccent, wireless_screen, true},
      {"PACKET CAPTURE", "Scoped traffic capture to PCAP", LV_SYMBOL_DOWNLOAD, "PCAP", kColorAmber, capture_screen, false},
      {"FINDINGS", "Review noteworthy discovered services", LV_SYMBOL_WARNING, "REVIEW", kColorAmber, findings_screen, false},
      {"EVIDENCE", "Exports, timeline and integrity", LV_SYMBOL_DIRECTORY, "STORE", kColorAccent, evidence_screen, false},
      {"VISION", "Capture photographic assessment evidence", LV_SYMBOL_IMAGE, "CAM", kColorAccent, vision_screen, false},
      {"LOCATION", "GNSS status and evidence geotagging", LV_SYMBOL_GPS, "GNSS", kColorAmber, location_screen, false},
  };
  constexpr lv_coord_t kGap = 10;
  const lv_coord_t card_width = (contentWidth(screen) - 3 * kGap) / 4;
  const lv_coord_t card_height = (contentHeight(screen) - kGap) / 2;
  for (int i = 0; i < 8; ++i) {
    lv_obj_t* card = createHomeAction(grid, i + 1, tools[i].title, tools[i].detail,
                                      tools[i].symbol, tools[i].badge, tools[i].color,
                                      tools[i].target, tools[i].scan);
    lv_obj_set_size(card, card_width, card_height);
    if (tools[i].target == findings_screen) {
      lv_obj_add_event_cb(card, [](lv_event_t* event) {
        g_findings_return = static_cast<lv_obj_t*>(lv_event_get_user_data(event));
      }, LV_EVENT_CLICKED, screen);
    }
  }
  return screen;
}

}  // namespace

int main() {
  std::signal(SIGINT, handleSignal);
  std::signal(SIGTERM, handleSignal);
  loadSession();

  lv_init();

  // Native panel mode is 568x1232 portrait (confirmed via drm_proof.cpp);
  // rotate to landscape for use with the keyboard kit attached, matching
  // k230_phone_ui's own 270-degree convention (and its own
  // lv_linux_drm_set_rotation() call, declared above - see that
  // declaration's comment for why this links against the vendor's
  // implementation rather than this project's own).
  lv_display_t* display = lv_linux_drm_create();
  if (display == nullptr) {
    std::fprintf(stderr, "DRM display create failed\n");
    return 1;
  }
  lv_linux_drm_set_rotation(display, LV_DISPLAY_ROTATION_270);
  if (lv_linux_drm_set_file(display, "/dev/dri/card0", -1) != LV_RESULT_OK) {
    std::fprintf(stderr, "DRM display init failed\n");
    return 1;
  }
  // Canaan's DRM backend performs the framebuffer rotation itself. Keep
  // LVGL's generic rotation disabled so it does not rotate input a second
  // time. This mirrors k230_phone_ui's apply_display_orientation().
  lv_display_set_rotation(display, LV_DISPLAY_ROTATION_0);
  lv_indev_t* touch = lv_evdev_create(LV_INDEV_TYPE_POINTER, "/dev/input/event1");
  if (touch != nullptr) {
    // Exact 270-degree touch transform used by Canaan/LILYGO's
    // k230_phone_ui. The controller reports approximately 0..1060 on its
    // native X axis and 0..2400 on Y. In landscape this maps to:
    //     logical_x = reverse(raw_y)
    //     logical_y = raw_x
    // lv_evdev applies calibration after swapping, hence this ordering.
    constexpr int kTouchMaxX = 1060;
    constexpr int kTouchMaxY = 2400;
    lv_evdev_set_swap_axes(touch, true);
    lv_evdev_set_calibration(touch, kTouchMaxY, 0, 0, kTouchMaxX);
  }

  // Make every interactive widget keyboard-focusable as it is created.
  // The TCA8418 is polled from the LVGL thread, so both the hardware
  // driver and widget updates remain single-threaded and race-free.
  lv_group_t* keyboard_group = lv_group_create();
  lv_group_set_default(keyboard_group);
  lv_indev_t* keyboard_indev = lv_indev_create();
  lv_indev_set_type(keyboard_indev, LV_INDEV_TYPE_KEYPAD);
  lv_indev_set_read_cb(keyboard_indev, readHardwareKeyboard);
  lv_indev_set_group(keyboard_indev, keyboard_group);
  g_keyboard_ready = g_keyboard.start();
  std::printf("reconclave k230 keyboard: %s\n", g_keyboard_ready ? "ready" : "not detected");

  using namespace reconclave::ui;

  lv_obj_t* home = createScreen();
  g_home_screen = home;
  addStatusBar(home, "RECONCLAVE", true);

  lv_obj_t* network_screen = buildNetworkScreen(home);
  lv_obj_t* wireless_screen = buildWirelessScreen(home);
  // Order doesn't matter for the cross-references between these three -
  // RECON's Findings button and both lists' host rows resolve their
  // targets from g_findings_screen/g_recon_screen/g_host_detail_return at
  // click time, not at build time (see addDynamicBackButton's comment).
  buildHostDetailScreen(home);
  buildFindingsScreen(home);
  lv_obj_t* recon_screen = buildReconScreen(home);
  lv_obj_t* vision_screen = buildVisionScreen(home);
  buildPhotoViewerScreen(home);  // sets g_photo_viewer_screen; opened only from VISION's preview.
  lv_obj_t* location_screen = buildLocationScreen(home);
  lv_obj_t* assessment_screen = buildAssessmentScreen(home);
  lv_obj_t* node_screen = buildNodeScreen(home);
  lv_obj_t* evidence_screen = buildEvidenceScreen(home);
  lv_obj_t* devices_screen = buildDevicesScreen(home);
  buildWifiPasswordScreen(home);  // sets g_wifi_password_screen; opened only from Wi-Fi setup's list.
  lv_obj_t* wifi_setup_screen = buildWifiSetupScreen(home);
  lv_obj_t* settings_screen = buildSettingsScreen(home, network_screen, wifi_setup_screen,
                                                  devices_screen, node_screen, location_screen);
  lv_obj_t* capture_screen = buildCaptureScreen(home);
  lv_obj_t* nmap_screen = buildNmapScreen(home);
  lv_obj_t* tools_screen = buildToolsScreen(home, recon_screen, nmap_screen, wireless_screen,
                                             capture_screen, g_findings_screen, evidence_screen,
                                             vision_screen, location_screen);
  g_ui_screens = {
      {"home", home}, {"tools", tools_screen}, {"network", network_screen},
      {"wifi-survey", wireless_screen}, {"wifi-setup", wifi_setup_screen},
      {"recon", recon_screen}, {"findings", g_findings_screen}, {"nmap", nmap_screen},
      {"packet", capture_screen}, {"evidence", evidence_screen}, {"vision", vision_screen},
      {"location", location_screen}, {"session", assessment_screen}, {"node", node_screen},
      {"device", devices_screen}, {"settings", settings_screen},
  };

  // Home is deliberately asymmetric: a persistent mission strip gives
  // context, while the larger command deck reserves visual weight for the
  // tools an operator actually launches in the field.
  lv_obj_t* mission = lv_obj_create(home);
  lv_obj_set_size(mission, 258, 464);
  lv_obj_align(mission, LV_ALIGN_TOP_LEFT, kSafeMargin, kSafeMargin + kStatusBarHeight + 12);
  lv_obj_set_style_bg_color(mission, lv_color_hex(0x091922), 0);
  lv_obj_set_style_border_color(mission, lv_color_hex(kColorAmber), 0);
  lv_obj_set_style_border_width(mission, 2, 0);
  lv_obj_set_style_border_side(mission, LV_BORDER_SIDE_LEFT, 0);
  lv_obj_set_style_radius(mission, 10, 0);
  lv_obj_set_style_pad_all(mission, 16, 0);
  lv_obj_clear_flag(mission, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t* mode = lv_label_create(mission);
  lv_label_set_text(mode, "ZETA // FIELD OPS");
  lv_obj_set_style_text_font(mode, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(mode, lv_color_hex(kColorAmber), 0);
  lv_obj_align(mode, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_t* mission_title = lv_label_create(mission);
  lv_label_set_text(mission_title, "MISSION\nCONTROL");
  lv_obj_set_style_text_font(mission_title, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(mission_title, lv_color_hex(kColorForeground), 0);
  lv_obj_align(mission_title, LV_ALIGN_TOP_LEFT, 0, 30);
  lv_obj_t* divider = lv_obj_create(mission);
  lv_obj_set_size(divider, lv_pct(100), 1);
  lv_obj_align(divider, LV_ALIGN_TOP_LEFT, 0, 94);
  lv_obj_set_style_bg_color(divider, lv_color_hex(kColorUnavailable), 0);
  lv_obj_set_style_border_width(divider, 0, 0);
  g_home_context = lv_label_create(mission);
  lv_obj_set_width(g_home_context, lv_pct(100));
  lv_label_set_long_mode(g_home_context, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_style_text_font(g_home_context, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(g_home_context, lv_color_hex(kColorSecondary), 0);
  lv_obj_align(g_home_context, LV_ALIGN_TOP_LEFT, 0, 112);
  g_home_readiness = lv_label_create(mission);
  lv_obj_set_width(g_home_readiness, lv_pct(100));
  lv_label_set_long_mode(g_home_readiness, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_style_text_font(g_home_readiness, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(g_home_readiness, lv_color_hex(kColorAccent), 0);
  lv_obj_align(g_home_readiness, LV_ALIGN_BOTTOM_LEFT, 0, -2);

  lv_obj_t* deck_title = lv_label_create(home);
  lv_label_set_text(deck_title, "COMMAND DECK  //  SELECT MODULE");
  lv_obj_set_style_text_font(deck_title, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(deck_title, lv_color_hex(kColorAccent), 0);
  lv_obj_align(deck_title, LV_ALIGN_TOP_LEFT, 294, 88);

  lv_obj_t* deck = lv_obj_create(home);
  lv_obj_set_size(deck, 918, 330);
  lv_obj_align(deck, LV_ALIGN_TOP_LEFT, 294, 116);
  lv_obj_set_style_bg_opa(deck, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(deck, 0, 0);
  lv_obj_set_style_pad_all(deck, 0, 0);
  lv_obj_set_style_pad_gap(deck, 12, 0);
  lv_obj_set_flex_flow(deck, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_clear_flag(deck, LV_OBJ_FLAG_SCROLLABLE);
  struct HomeSpec { const char* title; const char* detail; const char* symbol; const char* badge;
                    std::uint32_t color; lv_obj_t* target; bool scan; };
  const HomeSpec primary[] = {
      {"WI-FI OPS", "Survey APs and channel posture", LV_SYMBOL_WIFI, "SCAN", kColorAccent, wireless_screen, true},
      {"NMAP", "Discover hosts, ports and services", LV_SYMBOL_EYE_OPEN, "SCAN", kColorAmber, nmap_screen, false},
      {"PACKETS", "Capture scoped traffic to evidence", LV_SYMBOL_DOWNLOAD, "PCAP", kColorAccent, capture_screen, false},
      {"EVIDENCE", "Timeline, exports and integrity", LV_SYMBOL_DIRECTORY, "STORE", kColorAmber, evidence_screen, false},
      {"VISION", "Capture visual assessment evidence", LV_SYMBOL_IMAGE, "CAM", kColorAccent, vision_screen, false},
      {"REMOTE NODE", "Jobs, trust and coordination", LV_SYMBOL_LOOP, "RCN", kColorAmber, node_screen, false},
  };
  const lv_coord_t action_w = (918 - 24) / 3;
  const lv_coord_t action_h = (330 - 12) / 2;
  for (int i = 0; i < 6; ++i) {
    lv_obj_t* action = createHomeAction(deck, i + 1, primary[i].title, primary[i].detail,
                                        primary[i].symbol, primary[i].badge, primary[i].color,
                                        primary[i].target, primary[i].scan);
    lv_obj_set_size(action, action_w, action_h);
  }

  lv_obj_t* dock = lv_obj_create(home);
  lv_obj_set_size(dock, 918, 78);
  lv_obj_align(dock, LV_ALIGN_TOP_LEFT, 294, 470);
  lv_obj_set_style_bg_color(dock, lv_color_hex(0x091922), 0);
  lv_obj_set_style_border_width(dock, 0, 0);
  lv_obj_set_style_radius(dock, 10, 0);
  lv_obj_set_style_pad_all(dock, 10, 0);
  lv_obj_set_style_pad_gap(dock, 8, 0);
  lv_obj_set_flex_flow(dock, LV_FLEX_FLOW_ROW);
  lv_obj_clear_flag(dock, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t* utilities[] = {
      createHomeUtility(dock, LV_SYMBOL_LIST, "TOOLS", tools_screen),
      createHomeUtility(dock, LV_SYMBOL_GPS, "NETWORK", network_screen),
      createHomeUtility(dock, LV_SYMBOL_EDIT, "SESSION", assessment_screen),
      createHomeUtility(dock, LV_SYMBOL_SETTINGS, "DEVICE", devices_screen),
      createHomeUtility(dock, LV_SYMBOL_SETTINGS, "SETTINGS", settings_screen),
  };
  for (auto* utility : utilities) lv_obj_set_size(utility, (918 - 20 - 32) / 5, 58);

  refreshLiveDetails(nullptr);
  lv_timer_create(refreshLiveDetails, 3000, nullptr);
  lv_timer_create(pollHardwareKeyboard, 25, nullptr);
  lv_screen_load(home);

  std::printf("reconclave k230 ui: running\n");
  std::fflush(stdout);
  while (g_running) {
    lv_timer_handler();
    usleep(5000);
  }
  // g_preview_thread/g_wifi_op_thread are globals, joined here rather
  // than left for their destructors - a still-joinable std::thread at
  // static-destruction time calls std::terminate(), and SIGINT/SIGTERM
  // (handleSignal) can land while either worker is running regardless of
  // which screen was active.
  stopPreviewWorker();
  if (g_wifi_op_thread.joinable()) g_wifi_op_thread.join();
  if (g_nrf9151_enable_request != nullptr) gpiod_line_request_release(g_nrf9151_enable_request);
  if (g_nrf9151_gpio_chip != nullptr) gpiod_chip_close(g_nrf9151_gpio_chip);
  return 0;
}
