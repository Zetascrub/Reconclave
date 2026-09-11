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
#include "lvgl/src/drivers/display/drm/lv_linux_drm.h"
#include "lvgl/src/drivers/evdev/lv_evdev.h"

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
#include <arpa/inet.h>
#include <dirent.h>
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
#include <fstream>
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

namespace {

constexpr char kDeviceId[] = "rc-k230-poc";
constexpr char kFirmware[] = "reconclave-k230-0.2.0-alpha4";
constexpr char kEvidenceDir[] = "/root/reconclave/evidence";
constexpr char kSessionPath[] = "/root/reconclave/session.conf";
constexpr char kHostKnowledgePath[] = "/root/reconclave/hosts.json";
constexpr char kLocalCommandPath[] = "/run/reconclave/local-command";
// The GC2093 sensor's actual ISP capture output - confirmed via
// `v4l2-ctl --list-formats-ext` reporting "camera: ok" and a real (not
// black/garbage) test capture, as opposed to /dev/video0 (a memory-to-memory
// codec node, not a camera) or /dev/video1 (same ISP pipeline, unverified).
constexpr char kCameraDevice[] = "/dev/video2";

volatile sig_atomic_t g_running = 1;
void handleSignal(int) { g_running = 0; }

std::vector<reconclave::WifiObservation> g_last_wifi_scan;
lv_obj_t* g_wifi_list = nullptr;
lv_obj_t* g_wifi_status = nullptr;
lv_obj_t* g_evidence_status = nullptr;
lv_obj_t* g_evidence_summary = nullptr;
lv_obj_t* g_network_details = nullptr;
lv_obj_t* g_recon_details = nullptr;
lv_obj_t* g_vision_details = nullptr;
lv_obj_t* g_vision_status = nullptr;
lv_obj_t* g_vision_screen = nullptr;
lv_obj_t* g_vision_preview = nullptr;
lv_obj_t* g_vision_preview_hint = nullptr;
// Created once in buildVisionScreen(), paused/resumed (never deleted) as
// VISION is entered/left - see the screen's LV_EVENT_SCREEN_LOADED/
// LV_EVENT_SCREEN_UNLOADED handlers.
lv_timer_t* g_vision_preview_timer = nullptr;
lv_obj_t* g_photo_viewer_screen = nullptr;
lv_obj_t* g_photo_viewer_image = nullptr;
lv_obj_t* g_photo_viewer_caption = nullptr;
// Whatever was most recently captured (either a throwaway viewfinder frame
// or a saved evidence photo) - the VISION screen's preview thumbnail and
// the full-screen viewer both just show this, so "preview" and "capture"
// share one code path instead of tracking two separate images.
std::string g_last_photo_path;
constexpr char kPreviewPath[] = "/tmp/vision-preview.jpg";
// Fixed capture resolution (see camera_capture.cpp's -video_size), used to
// compute a "contain" scale for the viewer/thumbnail without needing to
// query the decoder for image dimensions first.
constexpr int kCaptureWidth = 1920;
constexpr int kCaptureHeight = 1080;
lv_obj_t* g_device_details = nullptr;
lv_obj_t* g_gnss_details = nullptr;
reconclave::GnssReceiver g_gnss;
std::string g_project_id{"UNASSIGNED"};
std::string g_engagement_id{"UNASSIGNED"};
std::string g_operator_id{"LOCAL"};
lv_obj_t* g_session_status = nullptr;
lv_obj_t* g_node_details = nullptr;
lv_obj_t* g_recon_action_status = nullptr;
bool g_lan_survey_armed = false;
std::time_t g_lan_survey_armed_at = 0;
std::string g_backlight_path;
int g_backlight_max = 255;

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
void showLastPhoto(const std::string& path, lv_obj_t* thumbnail_target);
void openPhotoViewer();
std::string readFirstLine(const std::string& path);
std::string readDeviceInfo();
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

std::string batteryText() {
  DIR* dir = opendir("/sys/class/power_supply");
  if (dir == nullptr) return LV_SYMBOL_BATTERY_EMPTY " N/A";
  std::string result = LV_SYMBOL_BATTERY_EMPTY " N/A";
  while (dirent* entry = readdir(dir)) {
    if (entry->d_name[0] == '.') continue;
    std::string base = std::string("/sys/class/power_supply/") + entry->d_name;
    std::string capacity = readFirstLine(base + "/capacity");
    if (capacity.empty()) continue;
    std::string status = readFirstLine(base + "/status");
    result = capacity + "%";
    if (status == "Charging") result = LV_SYMBOL_CHARGE + result;
    break;
  }
  closedir(dir);
  return result;
}

struct NetworkState {
  bool connected = false;
  std::string iface = "offline";
  std::string address = "No IPv4 address";
  std::string gateway = "No default route";
};

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
  char clock[8] = "--:--";
  std::time_t now = std::time(nullptr);
  if (std::tm* local = std::localtime(&now)) std::strftime(clock, sizeof(clock), "%H:%M", local);
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
    lv_obj_set_size(row, lv_pct(100), 58);
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

    lv_obj_t* row_label = lv_label_create(row);
    lv_label_set_text(row_label, name.c_str());
    lv_obj_set_width(row_label, lv_pct(46));
    lv_label_set_long_mode(row_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(row_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(row_label, lv_color_hex(reconclave::ui::kColorForeground), 0);
    lv_obj_align(row_label, LV_ALIGN_LEFT_MID, 16, 0);

    lv_obj_t* detail_label = lv_label_create(row);
    lv_label_set_text(detail_label, detail.c_str());
    lv_obj_set_width(detail_label, lv_pct(50));
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
std::thread g_preview_thread;
std::uint64_t g_preview_applied_generation = 0;  // UI-thread only, no lock needed.

void previewWorkerLoop() {
  while (g_preview_worker_running.load(std::memory_order_relaxed)) {
    const auto capture = reconclave::captureStill(kCameraDevice, kPreviewPath, 3000);
    {
      std::lock_guard<std::mutex> lock(g_preview_mutex);
      g_preview_last_ok = capture.ok;
      g_preview_last_error = capture.error;
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

// Cheap, non-blocking: runs every ~150ms on the UI thread while VISION is
// loaded (see buildVisionScreen), but does real work only on the ~once-a-
// second tick where the worker has actually produced a new frame.
void checkPreviewWorker(lv_timer_t*) {
  const auto generation = g_preview_generation.load(std::memory_order_acquire);
  if (generation == g_preview_applied_generation) return;  // nothing new yet
  g_preview_applied_generation = generation;

  bool ok = false;
  std::string error;
  {
    std::lock_guard<std::mutex> lock(g_preview_mutex);
    ok = g_preview_last_ok;
    error = g_preview_last_error;
  }
  if (!ok) {
    if (g_vision_status != nullptr) {
      lv_label_set_text(g_vision_status, ("Capture failed: " + error).c_str());
    }
    return;
  }
  showLastPhoto(kPreviewPath, g_vision_preview);
  if (g_vision_preview_hint != nullptr) lv_obj_add_flag(g_vision_preview_hint, LV_OBJ_FLAG_HIDDEN);
  if (g_vision_status != nullptr) {
    lv_label_set_text(g_vision_status,
        "Capturing frames in the background (thumbnail rendering is a known issue - see the"
        " warning above). Capture photo still saves correctly.");
  }
}

// Capture photo: a single, deliberate, synchronous shot straight into the
// evidence store. Unlike the live preview this is a one-off user action
// (like a real camera's shutter button), so a ~1s block here is expected
// shutter latency, not a background loop fighting the UI thread for time.
void captureEvidencePhoto() {
  if (g_vision_status == nullptr) return;
  stopPreviewWorker();  // don't contend with the worker for the camera device
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

void refreshEvidenceSummary() {
  if (g_evidence_summary == nullptr) return;
  DIR* dir = opendir(kEvidenceDir);
  int files = 0;
  uint64_t bytes = 0;
  std::string newest;
  if (dir != nullptr) {
    while (dirent* entry = readdir(dir)) {
      if (entry->d_name[0] == '.') continue;
      std::string path = std::string(kEvidenceDir) + "/" + entry->d_name;
      struct stat info {};
      if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) continue;
      ++files;
      bytes += static_cast<uint64_t>(info.st_size);
      if (newest.empty() || newest < entry->d_name) newest = entry->d_name;
    }
    closedir(dir);
  }
  std::string text = "Evidence store\n" + std::to_string(files) + " files  |  " + formatBytes(bytes);
  const reconclave::EvidenceStore evidence(kEvidenceDir, kDeviceId);
  const auto verification = evidence.verify();
  text += "\nTimeline: ";
  if (verification.valid) {
    text += "VERIFIED  |  " + std::to_string(verification.records) + " records";
  } else {
    text += "FAILED at record " + std::to_string(verification.first_invalid_record);
  }
  if (!newest.empty()) text += "\nLatest: " + newest;
  else text += "\nNo captures saved yet";
  lv_label_set_text(g_evidence_summary, text.c_str());
}

void refreshNetworkDetails() {
  if (g_network_details == nullptr) return;
  const NetworkState state = networkState();
  std::ostringstream out;
  out << (state.connected ? LV_SYMBOL_OK "  ONLINE" : LV_SYMBOL_CLOSE "  OFFLINE")
      << "     ATTACHED NETWORK   " << (attachedIpv4Network().empty() ? "n/a" : attachedIpv4Network()) << "\n\n";
  out << "INTERFACE       " << state.iface << "\n";
  out << "ADDRESS         " << state.address << "\n";
  out << "MAC             " << readFirstLine("/sys/class/net/" + state.iface + "/address") << "\n";
  out << "LINK            " << readFirstLine("/sys/class/net/" + state.iface + "/operstate");
  const std::string speed = readFirstLine("/sys/class/net/" + state.iface + "/speed");
  if (!speed.empty()) out << "  |  " << speed << " Mbps";
  out << "\nDEFAULT ROUTE   " << state.gateway << "\n";
  out << "NEIGHBOURS      " << arpNeighbourCount() << "\n";
  out << "RX / TX BYTES   " << readFirstLine("/sys/class/net/" + state.iface + "/statistics/rx_bytes")
      << " / " << readFirstLine("/sys/class/net/" + state.iface + "/statistics/tx_bytes");
  lv_label_set_text(g_network_details, out.str().c_str());
}

void refreshLiveDetails(lv_timer_t*) {
  g_gnss.poll();
  refreshStatusBars(nullptr);
  refreshNetworkDetails();
  if (g_device_details != nullptr) {
    lv_label_set_text(g_device_details, readDeviceInfo().c_str());
  }
  if (g_recon_details != nullptr) {
    const std::string detail = reconDashboardText();
    lv_label_set_text(g_recon_details, detail.c_str());
  }
  if (g_gnss_details != nullptr) {
    const auto& fix = g_gnss.fix();
    std::ostringstream details;
    details << (g_gnss.running() ? LV_SYMBOL_GPS "  " : LV_SYMBOL_WARNING "  ") << g_gnss.status() << "\n\n";
    if (fix.valid) {
      details << std::fixed << std::setprecision(7) << "LATITUDE     " << fix.latitude
              << "\nLONGITUDE    " << fix.longitude << std::setprecision(1)
              << "\nALTITUDE     " << fix.altitude_m << " m"
              << "\nSATELLITES   " << fix.satellites
              << "\nSPEED        " << fix.speed_knots << " kn"
              << "\nUTC          " << fix.utc << "\n\nEvidence geotagging active";
    } else {
      details << "No valid position yet\n\nMove outdoors with a clear sky view. A cold fix may take several minutes."
              << "\n\nWi-Fi evidence remains explicitly marked location_valid=false until a fix is acquired.";
    }
    lv_label_set_text(g_gnss_details, details.str().c_str());
  }
  if (g_node_details != nullptr) {
    std::string status = readTextFile("/run/reconclave/node-status");
    if (status.empty()) status = "NODE SERVICE OFFLINE\n\nNo runtime status is available.";
    lv_label_set_text(g_node_details, status.c_str());
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
    lv_obj_t* empty = lv_label_create(g_findings_list);
    lv_label_set_text(empty,
        "No findings yet. Findings appear once a service is identified as noteworthy or a known "
        "host's state changes.");
    lv_obj_set_width(empty, lv_pct(100));
    lv_label_set_long_mode(empty, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_color(empty, lv_color_hex(kColorSecondary), 0);
    lv_obj_set_style_text_font(empty, &lv_font_montserrat_14, 0);
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

  std::ostringstream out;
  out << "Device ID: " << kDeviceId << "\n";
  out << "Firmware: " << kFirmware << "\n";
  out << "Hostname: " << uts.nodename << "\n";
  out << "Kernel: " << uts.sysname << " " << uts.release << "\n";
  out << "Memory total:" << mem_total << "\n";
  out << "Memory available:" << mem_available << "\n";

  const double uptime_seconds = std::strtod(readFirstLine("/proc/uptime").c_str(), nullptr);
  out << "Uptime: " << static_cast<long>(uptime_seconds / 3600.0) << "h "
      << (static_cast<long>(uptime_seconds / 60.0) % 60) << "m\n";
  struct statvfs fs {};
  if (statvfs("/", &fs) == 0) {
    const uint64_t total = static_cast<uint64_t>(fs.f_blocks) * fs.f_frsize;
    const uint64_t free = static_cast<uint64_t>(fs.f_bavail) * fs.f_frsize;
  out << "Storage: " << formatBytes(total - free) << " used / " << formatBytes(total);
  }
  out << "\nSSH server: " << (readFirstLine("/var/run/sshd.pid").empty() ? "stopped" : "running")
      << " on port 22 (key only)";
  out << "\nSSH client: " << (access("/usr/bin/ssh", X_OK) == 0 ? "available" : "missing")
      << "  |  SCP/SFTP "
      << ((access("/usr/bin/scp", X_OK) == 0 && access("/usr/bin/sftp", X_OK) == 0) ? "available" : "missing");
  return out.str();
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
  addDynamicBackButton(screen, &g_recon_screen, home);

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

  lv_obj_t* preview_button = lv_button_create(screen);
  lv_obj_set_size(preview_button, 130, kBackButtonHeight);
  lv_obj_align(preview_button, LV_ALIGN_TOP_RIGHT, -kSafeMargin - 170 - 10,
               kSafeMargin + kStatusBarHeight + 8);
  lv_obj_set_style_bg_color(preview_button, lv_color_hex(kColorPanelLight), 0);
  lv_obj_set_style_border_color(preview_button, lv_color_hex(kColorAccent), 0);
  lv_obj_set_style_border_width(preview_button, 1, 0);
  lv_obj_set_style_radius(preview_button, 8, 0);
  lv_obj_set_style_shadow_width(preview_button, 0, 0);
  // Parked: the live preview's on-screen thumbnail is a known-broken issue
  // (see g_vision_details above) - disabled rather than wired to a
  // background loop nothing will render, until that's revisited.
  lv_obj_add_state(preview_button, LV_STATE_DISABLED);
  lv_obj_t* preview_label = lv_label_create(preview_button);
  lv_label_set_text(preview_label, LV_SYMBOL_REFRESH " Preview");
  lv_obj_set_style_text_color(preview_label, lv_color_hex(kColorAccent), 0);
  lv_obj_center(preview_label);

  // Left: diagnostics text. Right: a viewfinder/thumbnail - Preview fills
  // it with a throwaway frame before committing to a shot, Capture photo
  // fills it with the just-saved evidence image; tapping it either way
  // opens the full-screen viewer.
  constexpr lv_coord_t kPreviewWidth = 360;
  constexpr lv_coord_t kGap = 16;
  const lv_coord_t info_width = contentWidth(screen) - kPreviewWidth - kGap;

  lv_obj_t* card = lv_obj_create(screen);
  lv_obj_set_size(card, info_width, 210);
  lv_obj_align(card, LV_ALIGN_TOP_LEFT, kSafeMargin, kContentTop);
  lv_obj_set_style_bg_color(card, lv_color_hex(kColorPanelLight), 0);
  lv_obj_set_style_border_color(card, lv_color_hex(kColorAmber), 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_radius(card, 12, 0);
  lv_obj_set_style_pad_all(card, 18, 0);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

  g_vision_details = lv_label_create(card);
  std::ostringstream text;
  text << (camera_ready ? LV_SYMBOL_OK " CAMERA READY (GC2093)" : LV_SYMBOL_WARNING " CAMERA OFFLINE")
       << "\n\n" LV_SYMBOL_WARNING " Known issue: the on-screen preview thumbnail doesn't render yet"
          " (parked - see README's Vision section). Capture photo still saves a real JPEG into the"
          " evidence store (Evidence screen manifests and hashes it like any other file) - the"
          " capture itself works, only the on-screen preview is affected.\n"
          "K230 NPU available in BSP - OCR/QR decoding not built yet.";
  lv_label_set_text(g_vision_details, text.str().c_str());
  lv_obj_set_width(g_vision_details, lv_pct(100));
  lv_label_set_long_mode(g_vision_details, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_style_text_color(g_vision_details, lv_color_hex(kColorForeground), 0);
  lv_obj_set_style_text_font(g_vision_details, &lv_font_montserrat_16, 0);
  lv_obj_align(g_vision_details, LV_ALIGN_TOP_LEFT, 0, 0);

  lv_obj_t* preview_panel = lv_obj_create(screen);
  lv_obj_set_size(preview_panel, kPreviewWidth, 210);
  lv_obj_align(preview_panel, LV_ALIGN_TOP_RIGHT, -kSafeMargin, kContentTop);
  lv_obj_set_style_bg_color(preview_panel, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_color(preview_panel, lv_color_hex(kColorAccent), 0);
  lv_obj_set_style_border_width(preview_panel, 1, 0);
  lv_obj_set_style_radius(preview_panel, 12, 0);
  lv_obj_clear_flag(preview_panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(preview_panel, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(preview_panel, [](lv_event_t*) { openPhotoViewer(); }, LV_EVENT_CLICKED, nullptr);

  g_vision_preview = lv_image_create(preview_panel);
  lv_obj_center(g_vision_preview);
  g_vision_preview_hint = lv_label_create(preview_panel);
  lv_label_set_text(g_vision_preview_hint, "No frame yet - tap Preview to start the live feed");
  lv_obj_set_style_text_color(g_vision_preview_hint, lv_color_hex(kColorSecondary), 0);
  lv_obj_set_style_text_font(g_vision_preview_hint, &lv_font_montserrat_14, 0);
  lv_obj_center(g_vision_preview_hint);
  // g_vision_preview starts with no source, so it renders as nothing -
  // the hint label behind it is what's actually visible until the first
  // capture; setImageContain()/lv_image_set_src() draws over it after.

  g_vision_status = lv_label_create(screen);
  const std::string initial_status = camera_ready
      ? "Tap Preview to start a live feed, or Capture photo to save a shot to evidence."
      : "No camera device found at " + std::string(kCameraDevice);
  lv_label_set_text(g_vision_status, initial_status.c_str());
  lv_obj_set_width(g_vision_status, contentWidth(screen));
  lv_label_set_long_mode(g_vision_status, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_style_text_color(g_vision_status, lv_color_hex(kColorSecondary), 0);
  lv_obj_set_style_text_font(g_vision_status, &lv_font_montserrat_14, 0);
  lv_obj_align(g_vision_status, LV_ALIGN_TOP_LEFT, kSafeMargin, kContentTop + 210 + 12);

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
    if (!g_gnss.running()) g_gnss.start("/dev/ttyS3", error);
    refreshLiveDetails(nullptr);
  }, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* start_label = lv_label_create(start_button);
  lv_label_set_text(start_label, LV_SYMBOL_PLAY " Start GNSS");
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

  lv_obj_t* keyboard = lv_keyboard_create(screen);
  lv_obj_set_size(keyboard, contentWidth(screen), 218);
  lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, -kSafeMargin);
  lv_obj_set_style_bg_color(keyboard, lv_color_hex(kColorPanel), 0);

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
    lv_obj_set_size(fields[i], field_width, 48);
    lv_obj_align(fields[i], LV_ALIGN_TOP_LEFT, kSafeMargin + i * (field_width + 16), kContentTop + 28);
    lv_textarea_set_one_line(fields[i], true);
    lv_textarea_set_max_length(fields[i], 48);
    lv_textarea_set_text(fields[i], values[i]->c_str());
    lv_obj_set_style_bg_color(fields[i], lv_color_hex(kColorPanelLight), 0);
    lv_obj_set_style_text_color(fields[i], lv_color_hex(kColorForeground), 0);
    lv_obj_set_style_border_color(fields[i], lv_color_hex(kColorAccent), LV_STATE_FOCUSED);
    lv_obj_add_event_cb(fields[i], [](lv_event_t* event) {
      lv_keyboard_set_textarea(static_cast<lv_obj_t*>(lv_event_get_user_data(event)),
                               static_cast<lv_obj_t*>(lv_event_get_target(event)));
    }, LV_EVENT_FOCUSED, keyboard);
  }
  lv_keyboard_set_textarea(keyboard, fields[0]);

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

  g_session_status = lv_label_create(screen);
  lv_label_set_text(g_session_status, "Set field context before collecting assessment evidence");
  lv_obj_set_style_text_color(g_session_status, lv_color_hex(kColorSecondary), 0);
  lv_obj_set_style_text_font(g_session_status, &lv_font_montserrat_14, 0);
  lv_obj_set_width(g_session_status, contentWidth(screen));
  lv_label_set_long_mode(g_session_status, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_align(g_session_status, LV_ALIGN_TOP_LEFT, kSafeMargin, kContentTop + 84);
  return screen;
}

lv_obj_t* buildNodeScreen(lv_obj_t* home) {
  using namespace reconclave::ui;
  lv_obj_t* screen = createScreen();
  addStatusBar(screen, "NODE & JOBS", true);
  addBackButton(screen, home);
  lv_obj_t* card = lv_obj_create(screen);
  lv_obj_set_size(card, contentWidth(screen), contentHeight(screen));
  lv_obj_align(card, LV_ALIGN_TOP_LEFT, kSafeMargin, kContentTop);
  lv_obj_set_style_bg_color(card, lv_color_hex(kColorPanelLight), 0);
  lv_obj_set_style_border_color(card, lv_color_hex(kColorAccent), 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_radius(card, 12, 0);
  lv_obj_set_style_pad_all(card, 18, 0);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  g_node_details = lv_label_create(card);
  lv_obj_set_width(g_node_details, lv_pct(100));
  lv_label_set_long_mode(g_node_details, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_style_text_color(g_node_details, lv_color_hex(kColorForeground), 0);
  lv_obj_set_style_text_font(g_node_details, &lv_font_montserrat_16, 0);
  lv_obj_align(g_node_details, LV_ALIGN_TOP_LEFT, 0, 0);
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

  lv_obj_t* export_button = lv_button_create(screen);
  lv_obj_set_size(export_button, 260, 56);
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
  lv_obj_set_size(manifest_button, 260, 56);
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

  lv_obj_t* summary_card = lv_obj_create(screen);
  lv_obj_set_size(summary_card, 430, 150);
  lv_obj_align(summary_card, LV_ALIGN_TOP_RIGHT, -kSafeMargin, kContentTop);
  lv_obj_set_style_bg_color(summary_card, lv_color_hex(kColorPanelLight), 0);
  lv_obj_set_style_border_width(summary_card, 0, 0);
  lv_obj_set_style_radius(summary_card, 12, 0);
  lv_obj_set_style_pad_all(summary_card, 16, 0);
  lv_obj_clear_flag(summary_card, LV_OBJ_FLAG_SCROLLABLE);
  g_evidence_summary = lv_label_create(summary_card);
  lv_obj_set_width(g_evidence_summary, lv_pct(100));
  lv_obj_set_style_text_color(g_evidence_summary, lv_color_hex(kColorForeground), 0);
  lv_obj_set_style_text_font(g_evidence_summary, &lv_font_montserrat_16, 0);
  refreshEvidenceSummary();

  g_evidence_status = lv_label_create(screen);
  lv_label_set_text(g_evidence_status, "No export yet");
  lv_obj_set_style_text_color(g_evidence_status, lv_color_hex(kColorSecondary), 0);
  lv_obj_set_style_text_font(g_evidence_status, &lv_font_montserrat_14, 0);
  lv_obj_set_width(g_evidence_status, 360);
  lv_label_set_long_mode(g_evidence_status, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_align(g_evidence_status, LV_ALIGN_TOP_LEFT, kSafeMargin, kContentTop + 132);

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

  lv_obj_t* card = lv_obj_create(screen);
  lv_obj_set_size(card, contentWidth(screen), 250);
  lv_obj_align(card, LV_ALIGN_TOP_LEFT, kSafeMargin, kContentTop);
  lv_obj_set_style_bg_color(card, lv_color_hex(kColorPanelLight), 0);
  lv_obj_set_style_radius(card, 10, 0);
  lv_obj_set_style_border_width(card, 0, 0);
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
    if (g_device_details != nullptr) lv_label_set_text(g_device_details, readDeviceInfo().c_str());
  }, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* refresh_label = lv_label_create(refresh_button);
  lv_label_set_text(refresh_label, LV_SYMBOL_REFRESH " Refresh");
  lv_obj_set_style_text_color(refresh_label, lv_color_hex(kColorAccent), 0);
  lv_obj_center(refresh_label);

  lv_obj_t* card = lv_obj_create(screen);
  lv_obj_set_size(card, 700, contentHeight(screen));
  lv_obj_align(card, LV_ALIGN_TOP_LEFT, kSafeMargin, kContentTop);
  lv_obj_set_style_bg_color(card, lv_color_hex(kColorPanelLight), 0);
  lv_obj_set_style_radius(card, 10, 0);
  lv_obj_set_style_border_width(card, 0, 0);
  lv_obj_set_style_pad_all(card, 14, 0);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

  g_device_details = lv_label_create(card);
  lv_label_set_text(g_device_details, readDeviceInfo().c_str());
  lv_obj_set_style_text_color(g_device_details, lv_color_hex(kColorForeground), 0);
  lv_obj_set_style_text_font(g_device_details, &lv_font_montserrat_16, 0);
  lv_obj_align(g_device_details, LV_ALIGN_TOP_LEFT, 0, 0);

  lv_obj_t* control = lv_obj_create(screen);
  lv_obj_set_size(control, 450, contentHeight(screen));
  lv_obj_align(control, LV_ALIGN_TOP_RIGHT, -kSafeMargin, kContentTop);
  lv_obj_set_style_bg_color(control, lv_color_hex(kColorPanelLight), 0);
  lv_obj_set_style_radius(control, 10, 0);
  lv_obj_set_style_border_width(control, 0, 0);
  lv_obj_set_style_pad_all(control, 18, 0);
  lv_obj_clear_flag(control, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* heading = lv_label_create(control);
  lv_label_set_text(heading, "DISPLAY BRIGHTNESS");
  lv_obj_set_style_text_color(heading, lv_color_hex(kColorForeground), 0);
  lv_obj_set_style_text_font(heading, &lv_font_montserrat_16, 0);
  lv_obj_align(heading, LV_ALIGN_TOP_LEFT, 0, 0);

  lv_obj_t* slider = lv_slider_create(control);
  lv_obj_set_size(slider, lv_pct(100), 18);
  lv_obj_align(slider, LV_ALIGN_TOP_LEFT, 0, 54);
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

  lv_obj_t* hint = lv_label_create(control);
  lv_label_set_text(hint, "Drag to adjust the panel backlight.\nChanges apply immediately.");
  lv_obj_set_width(hint, lv_pct(100));
  lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_style_text_color(hint, lv_color_hex(kColorSecondary), 0);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
  lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 0, 96);

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

  using namespace reconclave::ui;

  lv_obj_t* home = createScreen();
  addStatusBar(home, "RECONCLAVE", true);

  // 3 columns x 2 rows, matching the §18 mockup's layout exactly (NETWORK
  // WIRELESS / RECON VISION / EVIDENCE DEVICES reads column-major in the
  // mockup; laid out row-major here since that's how createTileGrid
  // addresses cells, with the same six capabilities).
  lv_obj_t* grid = createTileGrid(home, 3, 3);

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

  struct TileSpec {
    const char* label;
    TileState state;
    lv_obj_t* target;
    const char* symbol;
    const char* subtitle;
  };
  // Flex-wrap places these left-to-right, top-to-bottom in this order -
  // see createTileGrid()'s comment for why flex rather than grid.
  const TileSpec tiles[] = {
      {"NETWORK", TileState::Live, network_screen, LV_SYMBOL_GPS, "Interface, route and connectivity"},
      {"WIRELESS", TileState::Live, wireless_screen, LV_SYMBOL_WIFI, "Scan APs and analyse channels"},
      {"RECON", TileState::Live, recon_screen, LV_SYMBOL_EYE_OPEN, "Survey LAN, hosts and services"},
      {"LOCATION", TileState::Live, location_screen, LV_SYMBOL_GPS, "Acquire evidence GNSS fix"},
      {"ASSESSMENT", TileState::Live, assessment_screen, LV_SYMBOL_EDIT, "Set project, scope and operator"},
      {"NODE", TileState::Live, node_screen, LV_SYMBOL_LOOP, "Jobs, trust and coordination"},
      {"EVIDENCE", TileState::Live, evidence_screen, LV_SYMBOL_DIRECTORY, "Export and verify manifest"},
      {"DEVICES", TileState::Live, devices_screen, LV_SYMBOL_SETTINGS, "Health, storage and brightness"},
      {"VISION", TileState::Live, vision_screen, LV_SYMBOL_IMAGE, "Camera diagnostics; capture pending"},
  };
  for (const auto& spec : tiles) {
    lv_obj_t* tile = createTile(grid, spec.label, spec.state, spec.symbol, spec.subtitle);
    if (spec.state == TileState::Live) {
      lv_obj_add_event_cb(
          tile,
          [](lv_event_t* event) {
            auto* target = static_cast<lv_obj_t*>(lv_event_get_user_data(event));
            lv_screen_load(target);
          },
          LV_EVENT_CLICKED, spec.target);
    }
  }

  refreshLiveDetails(nullptr);
  lv_timer_create(refreshLiveDetails, 3000, nullptr);
  lv_screen_load(home);

  std::printf("reconclave k230 ui: running\n");
  std::fflush(stdout);
  while (g_running) {
    lv_timer_handler();
    usleep(5000);
  }
  // g_preview_thread is a global, joined here rather than left for its
  // destructor - a still-joinable std::thread at static-destruction time
  // calls std::terminate(), and SIGINT/SIGTERM (handleSignal) can land
  // while the worker is running regardless of which screen was active.
  stopPreviewWorker();
  return 0;
}
