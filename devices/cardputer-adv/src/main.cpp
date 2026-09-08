#include "cap_radio_service.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <M5Cardputer.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <base64.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include "uptime.h"
#include "checked_file_print.h"
#include "ndef_content.h"
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <algorithm>
#include <string>
#include <vector>

#include <reconclave/node_registry.h>
#include "assets/zeta_title.h"
#include "generated_trust.h"
#include "network_host_scan_service.h"
#include "network_port_scan_service.h"

namespace {

constexpr char kFirmware[] = "0.1.0";
constexpr char kProtocol[] = "reconclave/1";
constexpr char kService[] = "reconclave";
constexpr char kTransport[] = "tcp";
constexpr char kAnnouncePath[] = "/reconclave/v1/announce";
constexpr char kMessagePath[] = "/reconclave/v1/message";
constexpr char kOtaUploadPath[] = "/reconclave/v1/ota-upload";
constexpr uint32_t kOtaArmTimeoutMs = 60000;
constexpr uint16_t kServerPort = 8766;
constexpr uint16_t kP4FallbackPort = 8765;
constexpr unsigned long kConnectTimeoutMs = 15000;
constexpr unsigned long kNodeExpiryMs = 45000;
constexpr int kMaxResponseBytes = 4096;
constexpr int kGroveRxPin = 2;
constexpr int kGroveTxPin = 1;
constexpr int kSdCsPin = 12;
constexpr int kSdMosiPin = 14;
constexpr int kSdClockPin = 40;
constexpr int kSdMisoPin = 39;
constexpr int kSdCompatibilityPin = 5;
constexpr uint32_t kSdFrequency = 4000000;
constexpr size_t kPeerKeyBytes = 32;
constexpr size_t kTagBytes = 16;

enum class ScreenState {
  ProvisionSsid,
  ProvisionPassword,
  Connecting,
  Home,
  Reconclave,
  NodeDetail,
  NodeCapabilities,
  Scout,
  HostDetail,
  PortResults,
  Observe,
  WifiResults,
  WifiChannels,
  WifiDetail,
  BleResults,
  BleDetail,
  NfcResults,
  NfcViewer,
  NfcPresets,
  NfcTools,
  NfcContent,
  NfcWriteConfirm,
  NfcEmulation,
  SubGhz,
  Evidence,
  EvidenceDetail,
  EvidencePreview,
  Projects,
  ProjectDetail,
  ProvisionProject,
  FieldKit,
  NetworkDashboard,
  System,
  Settings,
  SettingsConnectivity,
  SettingsDisplay,
  SettingsBoot,
  SettingsStorage,
  SettingsDevice,
  SettingsTrust,
  ConfirmForgetTrust,
  ProvisionEvidenceKey,
  ContextMenu,
};

enum class PortProfile { Web, Common, Extended };
enum class ScoutExecution : uint8_t { Auto, Single, Distributed, Consensus };
enum class DistributionStyle : uint8_t { Equal, Weighted };
enum class RecurringPolicy : uint8_t { Independent, Callback };
enum class UiTheme : uint8_t { Field, NightCity, Amber, Zeta };
constexpr unsigned kThemeCount = 4;
enum class NavigationStyle : uint8_t { Cards, List };
enum class IdleStyle : uint8_t { Off, Radar, Nodes, Zeta };
enum class BootSequence : uint8_t {
  CipherRain, SignalTrace, NodeBreach, PacketStorm, HexTunnel, RootAccess
};
enum class BootSpeed : uint8_t { Slow, Normal, Fast };
enum class ParticipationMode : uint8_t { NodeOnly, CoordinatorOnly, Both };

struct WifiObservation {
  String ssid;
  String bssid;
  int32_t rssi{0};
  int32_t channel{0};
  wifi_auth_mode_t auth{WIFI_AUTH_OPEN};
};

struct EvidenceFile {
  String name;
  uint64_t size{0};
};

struct ProjectScope {
  String projectId;
  String network;
  uint16_t revision{1};
};

struct BleObservation {
  String address;
  String name;
  String manufacturer;
  String services;
  int rssi{0};
  uint8_t addressType{0};
  uint8_t advertisementType{0};
  uint8_t serviceCount{0};
  size_t payloadLength{0};
  bool connectable{false};
};

struct RemoteCapability {
  String id;
  String permission{"public"};
  std::vector<String> features;
  uint8_t weight{1};
  uint16_t maxConcurrency{1};
};

struct RemoteNode {
  String deviceId;
  String deviceType;
  String firmware;
  String ip;
  uint16_t port{0};
  bool coordinator{false};
  std::vector<String> capabilities;
  std::vector<RemoteCapability> capabilityDescriptors;
  uint16_t networkMbps{0};
  bool persistentStorage{false};
  unsigned long lastSeenMs{0};
  String detail{"Press ENTER for system.info"};
};

struct RemoteScoutJob {
  String nodeId;
  uint8_t firstHost{1};
  uint8_t lastHost{254};
  uint16_t checked{0};
  uint16_t total{0};
  bool running{false};
  bool failed{false};
  String error;
  std::vector<String> hosts;
  uint16_t runCount{0};
};

bool nodeHasCapability(const RemoteNode& node, const char* capability) {
  for (const auto& advertised : node.capabilities) {
    if (advertised == capability) return true;
  }
  return false;
}

bool nodeCapabilityHasFeature(const RemoteNode& node, const char* capability,
                              const char* feature) {
  for (const auto& descriptor : node.capabilityDescriptors) {
    if (descriptor.id != capability) continue;
    return std::find(descriptor.features.begin(), descriptor.features.end(), String(feature)) !=
        descriptor.features.end();
  }
  return false;
}

Preferences preferences;
WebServer server(kServerPort);
reconclave::NodeRegistry registry;
M5Canvas uiCanvas(&M5Cardputer.Display);
bool uiCanvasReady{false};
ScreenState screen = ScreenState::Connecting;
RemoteNode p4;
std::vector<RemoteNode> remoteNodes;
std::vector<RemoteNode> evidenceCollectors;
String selectedNodeId;
size_t nodeSelection{0};
String wifiSsid;
String wifiPassword;
String input;
String notice;
String deviceId;
unsigned long connectStartedMs;
unsigned long messageSequence;
bool mdnsReady;
bool serverReady;
size_t selection;
std::vector<WifiObservation> wifiObservations;
std::vector<BleObservation> bleObservations;
std::vector<EvidenceFile> evidenceFiles;
std::vector<ProjectScope> projects;
String activeProjectId;
size_t projectSelection{0};
std::vector<String> discoveredHosts;
std::vector<String> localScoutHosts;
std::vector<String> previewLines;
size_t previewLine;
bool previewTruncated;
bool sdAvailable;
String fieldStatus;
bool wifiRestorePending;
unsigned nfcAction = 0;
String nfcPayload;
String nfcReadText, nfcReadUid;
bool nfcReadUrl = false;
size_t nfcReadRow = 0;
std::array<String, 16> nfcPresets;
std::array<bool, 16> nfcPresetUrls{};
std::array<bool, 16> nfcPresetOccupied{};
bool wifiScanning = false;
bool bleScanning = false;
bool wifiScanWasOff = false;
bool restoreAfterBle = false;
uint32_t wifiScanStartedMs = 0;
bool systemOpenedFromSettings{false};
uint8_t displayBrightness{160};
UiTheme uiTheme{UiTheme::Field};
NavigationStyle navigationStyle{NavigationStyle::Cards};
IdleStyle idleStyle{IdleStyle::Radar};
uint16_t screenTimeoutSeconds{60};
bool bootAnimationEnabled{true};
bool searchNodesOnBoot{false};
BootSequence bootSequence{BootSequence::CipherRain};
BootSpeed bootSpeed{BootSpeed::Normal};
bool interfaceSounds{true};
uint8_t interfaceVolume{72};
ParticipationMode participationMode{ParticipationMode::Both};
bool idleActive{false};
unsigned long lastInputMs{0};
unsigned long lastIdleFrameMs{0};
uint16_t idlePhase{0};
String scoutStatus{"Ready; choose a live scan provider"};
uint16_t scoutChecked;
uint16_t scoutTotal;
bool scoutRunning;
unsigned long lastScoutPollMs;
unsigned long lastScoutUiMs;
String scoutTargetId{"auto"};
String scoutRemoteNodeId;
ScoutExecution scoutExecution{ScoutExecution::Auto};
DistributionStyle distributionStyle{DistributionStyle::Weighted};
RecurringPolicy scoutRecurringPolicy{RecurringPolicy::Independent};
std::vector<RemoteScoutJob> remoteScoutJobs;
uint8_t localScoutFirstHost{1};
uint8_t localScoutLastHost{254};
bool localScoutAssigned{false};
bool scoutEvidenceSent{false};
uint16_t scoutIntervalMinutes{0};
unsigned long localScoutNextRunMs{0};
uint16_t scoutRunCount{0};
// Deterministic change detection: the host set as of the previous completed run,
// compared against each new run. Empty/unloaded means "no baseline yet" - the first
// run only seeds it rather than reporting every host as new.
std::vector<String> scoutBaseline;
bool scoutBaselineLoaded{false};
uint16_t scoutLastAppeared{0};
uint16_t scoutLastVanished{0};
// Set when a known Evidence Collector was skipped because no evidence key is
// configured. Surfaced on the Scout status line rather than `notice`, which is only
// ever rendered on the Wi-Fi connecting screen and would otherwise be silently lost.
bool scoutEvidenceKeyMissing{false};
unsigned long lastOutboxRetryMs{0};
uint16_t pendingEvidenceCount{0};
std::vector<String> disabledCapabilities;
PortProfile portProfile{PortProfile::Common};
ScreenState contextOrigin{ScreenState::Home};
size_t contextOriginSelection{0};
String scoutExecutor{"none"};
LocalHostScanService localHostScan;
LocalHostScanService remoteHostScan;
std::vector<String> remoteNodeScanHosts;
String remoteNodeJobId;
String remoteNodeJobOwner;
bool remoteNodeJobRecurring{false};
bool remoteNodeJobCancelled{false};
bool remoteNodeJobWasRunning{false};
uint32_t remoteNodeJobIntervalMs{0};
uint32_t remoteNodeJobNextRunMs{0};
uint16_t remoteNodeJobRunCount{0};
String remoteNodeJobProject;
uint16_t remoteNodeJobScopeRevision{0};
bool remoteNodeJobCallback{false};
String remoteNodeCallbackEndpoint;
uint8_t remoteNodeCallbackFailures{0};
unsigned long remoteNodeCallbackNextAttemptMs{0};
uint8_t remoteNodeFirstHost{1};
uint8_t remoteNodeLastHost{254};
NetworkPortScanService portScan;
String selectedHost;
size_t scoutSelection{0};
std::vector<uint16_t> openPorts;
String portStatus{"Ready for service check"};
bool portRunning{false};
static constexpr uint16_t kCommonPorts[] = {
    20, 21, 22, 23, 25, 53, 80, 110, 143, 443, 445, 3389, 8080};
static constexpr uint16_t kWebPorts[] = {80, 443, 8000, 8080, 8443, 8888};
static constexpr uint16_t kExtendedPorts[] = {
    20, 21, 22, 23, 25, 53, 67, 68, 69, 80, 110, 111, 123, 135, 137, 138,
    139, 143, 161, 389, 443, 445, 465, 587, 636, 993, 995, 1433, 1883,
    3306, 3389, 5432, 5900, 6379, 8000, 8080, 8443, 8888, 9100};

const char* portProfileLabel();
bool writeEvidenceRecord(JsonVariantConst evidence, String& errorMessage);
bool evidenceStorageAvailable();
void mountEvidence();
void distributeScoutEvidence(const std::vector<String>& hosts, const String& observer);
void detectAndRecordChanges(const std::vector<String>& hosts, const String& observer);
void loadScoutBaseline();
void saveScoutBaseline();
void attachEvidenceAuth(JsonObject payload, const String& destination, const String& requestId,
                        const char* capability);
void attachExecutionAuth(JsonObject payload, const String& destination, const String& requestId,
                         const char* capability);
bool sendEvidenceToNode(const RemoteNode& node, JsonVariantConst evidence);
bool queueEvidenceForNode(const RemoteNode& node, JsonVariantConst evidence);
void retryEvidenceOutbox();
void saveEvidenceCollectors();
void loadEvidenceCollectors();
void saveRemoteTask();
void loadRemoteTask();
void appendProjectAudit(const char* event, JsonVariantConst detail);
HardwareSerial groveSerial(2);
uint8_t peerKey[kPeerKeyBytes];
bool peerKeyValid;
uint8_t pendingPeerKey[kPeerKeyBytes];
bool pairingPending;
String trustedP4Id;
uint8_t evidenceKey[kPeerKeyBytes];
bool evidenceKeyValid;
uint8_t executionKey[kPeerKeyBytes];
bool executionKeyValid;
bool provisioningExecutionKey{false};

// Two-phase OTA session mirroring poe-p4's main.c: `fleet.ota.apply` (handleMessage, fully
// authenticated by executionRequestAuthenticated like net.discovery.scan) arms a session
// bound to one release's claimed SHA-256 and a fresh single-use token; the artifact bytes
// travel separately over kOtaUploadPath's raw-body WebServer route (see OtaUploadHandler)
// so this device never buffers a multi-hundred-KB-to-multi-MB base64 blob in RAM. The boot
// partition only switches once the streamed bytes' own SHA-256 matches what phase 1 signed.
struct OtaSession {
  bool armed = false;
  bool inProgress = false;
  bool committed = false;
  String token;
  String expectedSha256;
  String resultSha256Hex;
  String resultTagHex;
  size_t bytesWritten = 0;
  size_t maxBytes = 0;
  uint32_t armedAtMs = 0;
  mbedtls_md_context_t md;
  bool mdActive = false;
};
OtaSession otaSession;
// Set only while handling one /ota-upload request whose token didn't match a live armed
// session -- kept separate from OtaSession so a bogus/unauthenticated probe can never
// disturb a real armed session that a legitimate coordinator is about to upload against.
bool otaUploadRejectedAtStart = false;
const char* otaUploadRejectCode = nullptr;
uint32_t pendingRebootAtMs = 0;
// Nonces this Cardputer has itself accepted on storage.evidence.write, so a captured
// request can't be replayed against it. Small and bounded; no persistence needed.
std::vector<String> recentEvidenceNonces;
std::vector<String> recentExecutionNonces;
char nodeBootNonceHex[33]{};
constexpr size_t kRecentNonceCount = 16;
constexpr uint32_t kCoordinatorLeaseMs = 15000;
String groveP4Id;
String groveBootNonce;
bool groveP4Paired;
unsigned long lastGroveHeartbeatMs;
String pairingStatus{"P: pair over Grove"};
char groveLine[256];
size_t groveLineLength;

void saveRemoteTask() {
  preferences.putBool("rt_active", remoteNodeJobRecurring && !remoteNodeJobCancelled);
  preferences.putString("rt_id", remoteNodeJobId);
  preferences.putString("rt_owner", remoteNodeJobOwner);
  preferences.putUInt("rt_interval", remoteNodeJobIntervalMs);
  preferences.putUChar("rt_first", remoteNodeFirstHost);
  preferences.putUChar("rt_last", remoteNodeLastHost);
  preferences.putUShort("rt_runs", remoteNodeJobRunCount);
  preferences.putString("rt_project", remoteNodeJobProject);
  preferences.putUShort("rt_scope_rev", remoteNodeJobScopeRevision);
  preferences.putBool("rt_callback", remoteNodeJobCallback);
  preferences.putString("rt_cb_url", remoteNodeCallbackEndpoint);
  preferences.putUChar("rt_cb_fail", remoteNodeCallbackFailures);
}

void saveEvidenceCollectors() {
  String encoded;
  for (const auto& collector : evidenceCollectors) {
    if (!encoded.isEmpty()) encoded += '\n';
    encoded += collector.deviceId + "|" + collector.ip + "|" + String(collector.port);
  }
  preferences.putString("collectors", encoded);
}

void loadEvidenceCollectors() {
  const String encoded = preferences.getString("collectors", "");
  size_t start = 0;
  while (start < encoded.length()) {
    int end = encoded.indexOf('\n', start);
    if (end < 0) end = encoded.length();
    const String line = encoded.substring(start, end);
    const int first = line.indexOf('|');
    const int second = first >= 0 ? line.indexOf('|', first + 1) : -1;
    if (first > 0 && second > first + 1) {
      RemoteNode collector;
      collector.deviceId = line.substring(0, first);
      collector.ip = line.substring(first + 1, second);
      collector.port = static_cast<uint16_t>(line.substring(second + 1).toInt());
      collector.capabilities.push_back("storage.evidence.write");
      if (collector.port != 0) evidenceCollectors.push_back(collector);
    }
    start = static_cast<size_t>(end) + 1;
  }
}

void loadRemoteTask() {
  if (!preferences.getBool("rt_active", false)) return;
  remoteNodeJobId = preferences.getString("rt_id", "");
  remoteNodeJobOwner = preferences.getString("rt_owner", "");
  remoteNodeJobIntervalMs = preferences.getUInt("rt_interval", 0);
  remoteNodeFirstHost = preferences.getUChar("rt_first", 1);
  remoteNodeLastHost = preferences.getUChar("rt_last", 254);
  remoteNodeJobRunCount = preferences.getUShort("rt_runs", 0);
  remoteNodeJobProject = preferences.getString("rt_project", "");
  remoteNodeJobScopeRevision = preferences.getUShort("rt_scope_rev", 0);
  remoteNodeJobCallback = preferences.getBool("rt_callback", false);
  remoteNodeCallbackEndpoint = preferences.getString("rt_cb_url", "");
  remoteNodeCallbackFailures = preferences.getUChar("rt_cb_fail", 0);
  remoteNodeJobRecurring = !remoteNodeJobId.isEmpty() && remoteNodeJobIntervalMs >= 1000;
  remoteNodeJobCancelled = false;
  remoteNodeJobNextRunMs = remoteNodeJobRecurring && !remoteNodeJobCallback ? 1 : 0;
  remoteNodeCallbackNextAttemptMs = remoteNodeJobRecurring && remoteNodeJobCallback ? 1 : 0;
}

uint32_t crc32(const char* data, size_t length) {
  uint32_t crc = 0xffffffffU;
  for (size_t index = 0; index < length; ++index) {
    crc ^= static_cast<uint8_t>(data[index]);
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xedb88320U & static_cast<uint32_t>(-
          static_cast<int32_t>(crc & 1U)));
    }
  }
  return ~crc;
}

void hexEncode(char* destination, const uint8_t* source, size_t length) {
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < length; ++i) {
    destination[i * 2] = digits[source[i] >> 4];
    destination[i * 2 + 1] = digits[source[i] & 0x0f];
  }
  destination[length * 2] = '\0';
}

int hexNibble(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

bool hexDecode(uint8_t* destination, size_t length, const char* source) {
  if (source == nullptr || strlen(source) != length * 2) return false;
  for (size_t i = 0; i < length; ++i) {
    const int high = hexNibble(source[i * 2]);
    const int low = hexNibble(source[i * 2 + 1]);
    if (high < 0 || low < 0) return false;
    destination[i] = static_cast<uint8_t>((high << 4) | low);
  }
  return true;
}

bool constantTimeEqual(const uint8_t* left, const uint8_t* right, size_t length) {
  uint8_t difference = 0;
  for (size_t i = 0; i < length; ++i) difference |= left[i] ^ right[i];
  return difference == 0;
}

bool sha256Digest(const String& input, uint8_t output[32]) {
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  return info != nullptr && mbedtls_md(info, reinterpret_cast<const uint8_t*>(input.c_str()),
      input.length(), output) == 0;
}

bool jsonDigestHex(JsonVariantConst value, String& output) {
  String encoded;
  serializeJson(value, encoded);
  uint8_t digest[32];
  if (!sha256Digest(encoded, digest)) return false;
  char hex[65];
  hexEncode(hex, digest, sizeof(digest));
  output = hex;
  return true;
}

bool computeTagWithKey(const uint8_t* key, size_t keyLength, const String& message,
                       uint8_t output[kTagBytes]) {
  uint8_t full[32];
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (info == nullptr || mbedtls_md_hmac(info, key, keyLength,
      reinterpret_cast<const uint8_t*>(message.c_str()), message.length(), full) != 0) return false;
  memcpy(output, full, kTagBytes);
  return true;
}

bool computeTag(const String& message, uint8_t output[kTagBytes]) {
  if (!peerKeyValid) return false;
  return computeTagWithKey(peerKey, sizeof(peerKey), message, output);
}

bool computeEvidenceTag(const String& message, uint8_t output[kTagBytes]) {
  if (!evidenceKeyValid) return false;
  return computeTagWithKey(evidenceKey, sizeof(evidenceKey), message, output);
}

bool computeExecutionTag(const String& message, uint8_t output[kTagBytes]) {
  if (!executionKeyValid) return false;
  return computeTagWithKey(executionKey, sizeof(executionKey), message, output);
}

bool computeExecutionTagFull(const String& message, uint8_t output[32]) {
  if (!executionKeyValid) return false;
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  return info != nullptr && mbedtls_md_hmac(info, executionKey, sizeof(executionKey),
      reinterpret_cast<const uint8_t*>(message.c_str()), message.length(), output) == 0;
}

// Serialises `value` as compact JSON with object keys sorted lexicographically,
// matching the desktop coordinator's Python json.dumps(value, sort_keys=True,
// separators=(",", ":")) canonical form used to sign scope-delegation tokens and
// bind them to specific arguments (engagement_policy.py, reconclave_node.py; see
// docs/capabilities.md). Only the JSON subset the protocol actually carries here
// is supported -- null, bool, string, integer, array, string-keyed object -- and
// anything else fails closed rather than guessing a representation. When
// `excludeKey` is non-null and `value` is an object, that one top-level key is
// omitted (used to canonicalise a token without its own "tag", or arguments
// without the embedded "_scope_delegation").
bool canonicalJson(JsonVariantConst value, String& out, const char* excludeKey = nullptr) {
  if (value.isNull()) { out += "null"; return true; }
  if (value.is<bool>()) { out += (value.as<bool>() ? "true" : "false"); return true; }
  if (value.is<const char*>()) {
    String encoded;
    serializeJson(value, encoded);
    out += encoded;
    return true;
  }
  if (value.is<long long>()) {
    out += String(value.as<long long>());
    return true;
  }
  if (value.is<JsonArrayConst>()) {
    out += '[';
    bool first = true;
    for (JsonVariantConst item : value.as<JsonArrayConst>()) {
      if (!first) out += ',';
      first = false;
      if (!canonicalJson(item, out)) return false;
    }
    out += ']';
    return true;
  }
  if (value.is<JsonObjectConst>()) {
    JsonObjectConst object = value.as<JsonObjectConst>();
    std::vector<String> keys;
    for (JsonPairConst pair : object) {
      const String key(pair.key().c_str());
      if (excludeKey != nullptr && key == excludeKey) continue;
      for (size_t i = 0; i < key.length(); ++i) {
        const char c = key[i];
        const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_';
        if (!safe) return false;
      }
      keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end());
    out += '{';
    bool first = true;
    for (const String& key : keys) {
      if (!first) out += ',';
      first = false;
      out += '"'; out += key; out += "\":";
      if (!canonicalJson(object[key.c_str()], out)) return false;
    }
    out += '}';
    return true;
  }
  return false;
}

bool parseCidr(const String& text, uint32_t& base, uint8_t& prefix) {
  const int slash = text.indexOf('/');
  if (slash <= 0) return false;
  IPAddress address;
  if (!address.fromString(text.substring(0, slash))) return false;
  const int prefixValue = text.substring(slash + 1).toInt();
  if (prefixValue < 0 || prefixValue > 32) return false;
  base = (static_cast<uint32_t>(address[0]) << 24) | (static_cast<uint32_t>(address[1]) << 16) |
      (static_cast<uint32_t>(address[2]) << 8) | static_cast<uint32_t>(address[3]);
  prefix = static_cast<uint8_t>(prefixValue);
  return true;
}

uint32_t maskForPrefix(uint8_t prefix) {
  return prefix == 0 ? 0 : (0xffffffffu << (32 - prefix));
}

bool cidrSubnetOf(uint32_t targetBase, uint8_t targetPrefix, uint32_t otherBase, uint8_t otherPrefix) {
  if (targetPrefix < otherPrefix) return false;
  const uint32_t mask = maskForPrefix(otherPrefix);
  return (targetBase & mask) == (otherBase & mask);
}

bool cidrOverlaps(uint32_t aBase, uint8_t aPrefix, uint32_t bBase, uint8_t bPrefix) {
  const uint32_t mask = maskForPrefix(aPrefix < bPrefix ? aPrefix : bPrefix);
  return (aBase & mask) == (bBase & mask);
}

// Checks a requested target network against a delegated token's included/excluded
// CIDR lists the same way EngagementPolicy.authorize does on the desktop
// coordinator: the target must be contained by at least one included network and
// must not overlap any excluded network.
bool scopeNetworkAuthorised(JsonVariantConst included, JsonVariantConst excluded,
                            const String& targetText) {
  uint32_t targetBase;
  uint8_t targetPrefix;
  if (targetText.isEmpty() || !parseCidr(targetText, targetBase, targetPrefix)) return false;
  bool includedMatch = false;
  if (included.is<JsonArrayConst>()) {
    for (JsonVariantConst entry : included.as<JsonArrayConst>()) {
      if (!entry.is<const char*>()) continue;
      uint32_t base;
      uint8_t prefix;
      if (parseCidr(String(entry.as<const char*>()), base, prefix) &&
          cidrSubnetOf(targetBase, targetPrefix, base, prefix)) { includedMatch = true; break; }
    }
  }
  if (!includedMatch) return false;
  if (excluded.is<JsonArrayConst>()) {
    for (JsonVariantConst entry : excluded.as<JsonArrayConst>()) {
      if (!entry.is<const char*>()) continue;
      uint32_t base;
      uint8_t prefix;
      if (parseCidr(String(entry.as<const char*>()), base, prefix) &&
          cidrOverlaps(targetBase, targetPrefix, base, prefix)) return false;
    }
  }
  return true;
}

// Verifies a delegated engagement-scope token embedded at arguments["_scope_delegation"],
// mirroring EngagementPolicy.delegate/ReconclaveNode.verify_scope_delegation on the
// desktop coordinator (engagement_policy.py, reconclave_node.py). The token is signed
// with the same execution key used to authenticate the outer request, is bound to this
// exact capability, destination node, and argument set, and lists the networks it
// authorises. Firmware has no wall-clock time (no NTP sync), so unlike the desktop
// verifier this cannot check issued_at_ms/expires_at_ms against "now"; it only checks
// the token's internal consistency (expiry strictly after issue, and within the
// 5-minute lifetime ceiling the coordinator itself enforces when minting tokens).
// Absolute freshness is still covered by the outer request's boot_nonce-bound replay
// protection, which already prevents an old signed request -- token included -- from
// being resent after this device reboots or replayed within the same boot session.
bool scopeDelegationValid(JsonVariantConst arguments, const char* capability,
                          const char*& errorCode, const char*& errorMessage) {
  JsonVariantConst token = arguments["_scope_delegation"];
  if (!executionKeyValid || !token.is<JsonObjectConst>()) {
    errorCode = "SCOPE_REQUIRED";
    errorMessage = "signed scope delegation is required";
    return false;
  }
  const char* tagHex = token["tag"] | "";
  uint8_t suppliedTag[32];
  if (tagHex == nullptr || strlen(tagHex) != sizeof(suppliedTag) * 2 ||
      !hexDecode(suppliedTag, sizeof(suppliedTag), tagHex)) {
    errorCode = "SCOPE_INVALID";
    errorMessage = "scope delegation signature is invalid";
    return false;
  }
  String unsignedCanonical;
  if (!canonicalJson(token, unsignedCanonical, "tag")) {
    errorCode = "SCOPE_INVALID";
    errorMessage = "scope delegation could not be canonicalised";
    return false;
  }
  uint8_t expectedTag[32];
  if (!computeExecutionTagFull(unsignedCanonical, expectedTag) ||
      !constantTimeEqual(expectedTag, suppliedTag, sizeof(expectedTag))) {
    errorCode = "SCOPE_INVALID";
    errorMessage = "scope delegation signature is invalid";
    return false;
  }
  const String tokenCapability = token["capability"] | "";
  const String tokenDestination = token["destination_node"] | "";
  if (tokenCapability != capability || tokenDestination != deviceId) {
    errorCode = "SCOPE_INVALID";
    errorMessage = "scope delegation does not match this operation";
    return false;
  }
  String originalCanonical;
  if (!canonicalJson(arguments, originalCanonical, "_scope_delegation")) {
    errorCode = "SCOPE_INVALID";
    errorMessage = "scope delegation could not be canonicalised";
    return false;
  }
  uint8_t argumentsDigest[32];
  char argumentsDigestHex[65];
  if (!sha256Digest(originalCanonical, argumentsDigest)) {
    errorCode = "SCOPE_INVALID";
    errorMessage = "scope delegation could not be canonicalised";
    return false;
  }
  hexEncode(argumentsDigestHex, argumentsDigest, sizeof(argumentsDigest));
  const String tokenArgumentsDigest = token["arguments_digest"] | "";
  if (tokenArgumentsDigest != argumentsDigestHex) {
    errorCode = "SCOPE_INVALID";
    errorMessage = "scope delegation does not match this operation";
    return false;
  }
  const long long issuedAt = token["issued_at_ms"] | -1;
  const long long expiresAt = token["expires_at_ms"] | -1;
  if (issuedAt < 0 || expiresAt <= issuedAt || (expiresAt - issuedAt) > 300000) {
    errorCode = "SCOPE_EXPIRED";
    errorMessage = "scope delegation has an invalid lifetime";
    return false;
  }
  if (!scopeNetworkAuthorised(token["included_networks"], token["excluded_networks"],
                              String(arguments["network"] | ""))) {
    errorCode = "SCOPE_DENIED";
    errorMessage = "target is outside delegated scope";
    return false;
  }
  return true;
}

bool evidenceNonceFresh(const String& nonce) {
  if (std::find(recentEvidenceNonces.begin(), recentEvidenceNonces.end(), nonce) !=
      recentEvidenceNonces.end()) {
    return false;
  }
  recentEvidenceNonces.push_back(nonce);
  if (recentEvidenceNonces.size() > kRecentNonceCount) {
    recentEvidenceNonces.erase(recentEvidenceNonces.begin());
  }
  return true;
}

uint16_t colour(uint8_t red, uint8_t green, uint8_t blue) {
  // Remap the shared semantic palette so every screen inherits the theme.
  if (uiTheme == UiTheme::Zeta) {
    if (red == 5 && green == 10 && blue == 16) return M5Cardputer.Display.color565(3, 15, 23);
    if (red == 9 && green == 28 && blue == 39) return M5Cardputer.Display.color565(14, 34, 46);
    if (red == 11 && green == 30 && blue == 40) return M5Cardputer.Display.color565(20, 42, 54);
    if (red == 22 && green == 66 && blue == 72) return M5Cardputer.Display.color565(0, 70, 88);
    if (red == 80 && green == 230 && blue == 190) return M5Cardputer.Display.color565(0, 225, 235);
    if (red == 255 && green == 190 && blue == 70) return M5Cardputer.Display.color565(255, 170, 28);
    if (red == 130 && green == 155 && blue == 160) return M5Cardputer.Display.color565(172, 188, 197);
    if (red == 150 && green == 170 && blue == 175) return M5Cardputer.Display.color565(217, 191, 153);
    if (red == 35 && green == 118 && blue == 112) return M5Cardputer.Display.color565(0, 124, 153);
    if (red == 30 && green == 105 && blue == 105) return M5Cardputer.Display.color565(0, 117, 147);
    if (red == 24 && green == 64 && blue == 70) return M5Cardputer.Display.color565(40, 72, 91);
  }
  if (uiTheme == UiTheme::NightCity) {
    if (red == 5 && green == 10 && blue == 16) return M5Cardputer.Display.color565(5, 5, 12);
    if (red == 9 && green == 28 && blue == 39) return M5Cardputer.Display.color565(18, 12, 30);
    if (red == 11 && green == 30 && blue == 40) return M5Cardputer.Display.color565(22, 15, 35);
    if (red == 22 && green == 66 && blue == 72) return M5Cardputer.Display.color565(55, 24, 70);
    if (red == 80 && green == 230 && blue == 190) return M5Cardputer.Display.color565(255, 222, 70);
    if (red == 255 && green == 190 && blue == 70) return M5Cardputer.Display.color565(255, 80, 180);
    if (green > 60 && blue >= 70) return M5Cardputer.Display.color565(45, 155, 190);
  } else if (uiTheme == UiTheme::Amber) {
    if (red == 5 && green == 10 && blue == 16) return M5Cardputer.Display.color565(12, 8, 3);
    if (red == 9 && green == 28 && blue == 39) return M5Cardputer.Display.color565(34, 22, 7);
    if (red == 11 && green == 30 && blue == 40) return M5Cardputer.Display.color565(39, 26, 8);
    if (red == 22 && green == 66 && blue == 72) return M5Cardputer.Display.color565(72, 46, 9);
    if (red == 80 && green == 230 && blue == 190) return M5Cardputer.Display.color565(255, 184, 55);
    if (red == 255 && green == 190 && blue == 70) return M5Cardputer.Display.color565(255, 112, 40);
    if (green > 60 && blue >= 70) return M5Cardputer.Display.color565(160, 105, 35);
  }
  return M5Cardputer.Display.color565(red, green, blue);
}

uint16_t themeTextColour() {
  return uiTheme == UiTheme::Zeta ? M5Cardputer.Display.color565(255, 242, 215) : TFT_WHITE;
}

enum class UiCue : uint8_t { Move, Open, Back, Confirm, Warning, Boot };

void playUiCue(UiCue cue) {
  if (!interfaceSounds) return;
  uint16_t frequency = 880;
  uint16_t duration = 18;
  if (uiTheme == UiTheme::NightCity) frequency = 1047;
  else if (uiTheme == UiTheme::Amber) frequency = 659;
  switch (cue) {
    case UiCue::Move: break;
    case UiCue::Open: frequency += 180; duration = 26; break;
    case UiCue::Back: frequency = frequency > 220 ? frequency - 220 : frequency; duration = 24; break;
    case UiCue::Confirm: frequency += 360; duration = 42; break;
    case UiCue::Warning: frequency = 294; duration = 70; break;
    case UiCue::Boot: frequency += 480; duration = 80; break;
  }
  M5Cardputer.Speaker.setVolume(interfaceVolume);
  M5Cardputer.Speaker.tone(frequency, duration);
}

const char* themeLabel() {
  if (uiTheme == UiTheme::NightCity) return "NIGHT CITY";
  if (uiTheme == UiTheme::Amber) return "AMBER CRT";
  if (uiTheme == UiTheme::Zeta) return "ZETA MASCOT";
  return "NEON GRID";
}

void drawDeckMotif() {
  auto& display = uiCanvas;
  if (uiTheme == UiTheme::Zeta) {
    display.drawLine(0, 35, 8, 27, colour(80, 230, 190));
    display.drawLine(0, 43, 8, 35, colour(35, 118, 112));
    display.drawLine(231, 114, 239, 106, colour(255, 190, 70));
  } else if (uiTheme == UiTheme::NightCity) {
    display.drawFastVLine(2, 27, 87, colour(255, 190, 70));
    display.drawFastHLine(2, 113, 20, colour(255, 190, 70));
  } else if (uiTheme == UiTheme::Amber) {
    for (int y = 27; y < 118; y += 4)
      display.drawFastHLine(0, y, 2, colour(35, 118, 112));
  } else {
    display.drawLine(0, 32, 8, 24, colour(35, 118, 112));
    display.drawLine(232, 118, 239, 111, colour(35, 118, 112));
  }
}

// Fit labels to the actual pixel budget without leaving half a UTF-8 codepoint.
String fitUiText(String value, int width) {
  if (uiCanvas.textWidth(value.c_str()) <= width) return value;
  while (!value.isEmpty() && uiCanvas.textWidth((value + "...").c_str()) > width) {
    size_t end = value.length() - 1;
    while (end && (static_cast<unsigned char>(value[end]) & 0xC0) == 0x80) --end;
    value.remove(end);
  }
  return value + "...";
}

bool showTabHint() {
  return screen != ScreenState::ProvisionSsid && screen != ScreenState::ProvisionPassword &&
      screen != ScreenState::ProvisionProject && screen != ScreenState::ProvisionEvidenceKey &&
      screen != ScreenState::Connecting && screen != ScreenState::NfcTools &&
      screen != ScreenState::NfcViewer && screen != ScreenState::NfcPresets &&
      screen != ScreenState::NfcWriteConfirm && screen != ScreenState::NfcEmulation;
}

void drawListPosition(size_t first, size_t visible, size_t count) {
  if (count <= visible) return;
  constexpr int top = 27, height = 87;
  const int thumb = std::max(5, int(height * visible / count));
  const int offset = int((height - thumb) * first / (count - visible));
  uiCanvas.drawFastVLine(237, top, height, colour(24, 64, 70));
  uiCanvas.fillRect(236, top + offset, 3, thumb, colour(80, 230, 190));
}

void header(const char* title) {
  auto& display = uiCanvas;
  display.setTextWrap(false);
  display.fillScreen(colour(5, 10, 16));
  display.fillRect(0, 0, display.width(), 23, colour(9, 28, 39));
  display.drawFastHLine(0, 23, display.width(), colour(30, 105, 105));
  drawDeckMotif();
  display.setTextColor(colour(80, 230, 190));
  display.setTextSize(1);
  display.setCursor(7, 8);
  display.print(fitUiText(title, 160));
  display.setTextColor(WiFi.status() == WL_CONNECTED ? colour(80, 230, 190)
                                                     : colour(255, 190, 70));
  display.fillRoundRect(173, 5, 28, 12, 3, colour(11, 30, 40));
  display.setCursor(177, 8);
  display.print(WiFi.status() == WL_CONNECTED ? "NET" : "OFF");
  display.setTextColor(peerKeyValid ? colour(80, 230, 190) : colour(130, 155, 160));
  display.fillRoundRect(204, 5, 31, 12, 3, colour(11, 30, 40));
  display.setCursor(208, 8);
  display.print(peerKeyValid ? "KEY" : "---");
}

void footer(const char* text) {
  auto& display = uiCanvas;
  display.fillRect(0, 119, display.width(), 16, colour(9, 28, 39));
  display.drawFastHLine(0, 118, display.width(), colour(24, 64, 70));
  display.setTextColor(colour(150, 170, 175));
  display.setCursor(6, 123);
  display.print(fitUiText(text, showTabHint() ? 196 : 228));
  if (!showTabHint()) return;
  display.fillRoundRect(207, 121, 29, 11, 3, colour(22, 66, 72));
  display.setTextColor(colour(80, 230, 190));
  display.setCursor(211, 123);
  display.print("TAB");
}

void drawMenu(const char* title, const char* const* items, size_t count,
              const char* help = "Enter: open   Q/Esc: back") {
  header(title);
  auto& display = uiCanvas;
  if (count == 0) return;
  if (selection >= count) selection = count - 1;
  const size_t first = count > 6 ? std::min(selection >= 5 ? selection - 4 : size_t(0), count - 6) : 0;
  for (size_t row = 0; row < 6 && first + row < count; ++row) {
    const size_t index = first + row;
    const int y = 27 + static_cast<int>(row) * 15;
    if (index == selection) {
      display.fillRoundRect(7, y - 2, 226, 14, 3, colour(22, 66, 72));
      display.drawFastVLine(7, y, 10, colour(80, 230, 190));
      display.setTextColor(colour(80, 230, 190));
      display.setCursor(12, y);
      display.printf("%02u", static_cast<unsigned>(index + 1));
    } else {
      display.setTextColor(colour(130, 155, 160));
      display.setCursor(12, y);
      display.printf("%02u", static_cast<unsigned>(index + 1));
    }
    display.setTextColor(index == selection ? colour(80, 230, 190) : themeTextColour());
    display.setCursor(34, y);
    display.print(fitUiText(items[index], 195));
  }
  drawListPosition(first, 6, count);
  footer(help);
}

void drawProvision(const char* label, bool secret) {
  header("RECONCLAVE SETUP");
  auto& display = uiCanvas;
  display.setTextColor(themeTextColour());
  display.setCursor(8, 36);
  display.printf("Enter Wi-Fi %s:", label);
  display.drawRect(7, 51, 226, 28, colour(50, 110, 120));
  display.setCursor(12, 61);
  const size_t visibleStart = input.length() > 35 ? input.length() - 35 : 0;
  if (secret) {
    for (size_t i = visibleStart; i < input.length(); ++i) display.print('*');
  } else {
    display.print(input.substring(visibleStart));
  }
  display.setTextColor(colour(150, 170, 175));
  display.setCursor(8, 91);
  display.print("ENTER save   ESC offline/cancel");
  display.setCursor(8, 106);
  display.print("Credentials stay in device NVS");
}

void drawConnecting() {
  header("RECONCLAVE");
  auto& display = uiCanvas;
  display.setTextColor(themeTextColour());
  display.setCursor(8, 38);
  display.print(("Connecting to " + wifiSsid).substring(0, 37));
  display.setCursor(8, 56);
  display.printf("Wi-Fi: %s", WiFi.status() == WL_CONNECTED ? "connected" : "waiting...");
  display.setTextColor(colour(150, 170, 175));
  display.setCursor(8, 103);
  display.print("W: Wi-Fi setup   Q/Esc: offline");
  if (!notice.isEmpty()) {
    display.setTextColor(colour(255, 190, 70));
    display.setCursor(8, 78);
    display.print(notice.substring(0, 37));
  }
  footer("W: change Wi-Fi");
}

void drawDashboard() {
  header("RECONCLAVE / NODES");
  auto& display = uiCanvas;
  const size_t count = remoteNodes.size() + 1;
  if (selection >= count) selection = count - 1;
  const size_t first = selection >= 4 ? selection - 3 : 0;
  for (size_t row = 0; row < 4 && first + row < count; ++row) {
    const size_t index = first + row;
    const bool local = index == 0;
    const RemoteNode* node = local ? nullptr : &remoteNodes[index - 1];
    const int y = 29 + static_cast<int>(row) * 22;
    if (index == selection) display.fillRoundRect(4, y - 3, 232, 20, 4, colour(22, 66, 72));
    display.setTextColor(index == selection ? colour(80, 230, 190) : themeTextColour());
    display.setCursor(8, y);
    display.print(index == selection ? '>' : ' ');
    display.setCursor(18, y);
    display.print(local ? "THIS CARDPUTER" : node->deviceType.substring(0, 17));
    display.setCursor(150, y);
    display.print(String(local ? "COORDINATOR" : node->ip).substring(0, 14));
    display.setTextColor(colour(130, 155, 160));
    display.setCursor(18, y + 10);
    const String shownId = local ? deviceId : node->deviceId;
    display.print(shownId.length() > 32 ? shownId.substring(shownId.length() - 32) : shownId);
  }
  footer("UP/DOWN: nodes  Enter: manage  Q:back");
}

RemoteNode* selectedRemoteNode() {
  if (selectedNodeId.isEmpty() || selectedNodeId == deviceId) return nullptr;
  for (auto& node : remoteNodes) if (node.deviceId == selectedNodeId) return &node;
  return nullptr;
}

String capabilityPolicyKey(const String& nodeId, const String& capability) {
  return nodeId + "|" + capability;
}

bool capabilityEnabled(const String& nodeId, const String& capability) {
  const String key = capabilityPolicyKey(nodeId, capability);
  return std::find(disabledCapabilities.begin(), disabledCapabilities.end(), key) ==
      disabledCapabilities.end();
}

void saveCapabilityPolicy() {
  String encoded;
  for (const auto& key : disabledCapabilities) {
    if (!encoded.isEmpty()) encoded += '\n';
    encoded += key;
  }
  preferences.putString("cap_off", encoded);
}

void toggleCapability(const String& nodeId, const String& capability) {
  const String key = capabilityPolicyKey(nodeId, capability);
  auto existing = std::find(disabledCapabilities.begin(), disabledCapabilities.end(), key);
  if (existing == disabledCapabilities.end()) disabledCapabilities.push_back(key);
  else disabledCapabilities.erase(existing);
  saveCapabilityPolicy();
}

void drawNodeDetail() {
  header("RECONCLAVE / NODE");
  auto& display = uiCanvas;
  const bool local = selectedNodeId == deviceId;
  RemoteNode* node = selectedRemoteNode();
  display.setTextColor(colour(80, 230, 190));
  display.setCursor(8, 31);
  display.print(String(local ? "THIS CARDPUTER" :
      (node ? node->deviceType.c_str() : "NODE UNAVAILABLE")).substring(0, 37));
  display.setTextColor(themeTextColour());
  display.setCursor(8, 49);
  display.print((local ? deviceId : selectedNodeId).substring(0, 37));
  display.setCursor(8, 67);
  display.print((String("Address: ") + (local ? WiFi.localIP().toString() :
      (node ? node->ip : "offline"))).substring(0, 37));
  display.setCursor(8, 84);
  display.print((String("Firmware: ") + (local ? kFirmware :
      (node ? node->firmware : "unknown"))).substring(0, 37));
  display.setTextColor(colour(150, 170, 175));
  display.setCursor(8, 101);
  display.print(local ? "Roles: coordinator + node" :
      (node && node->deviceId == p4.deviceId ? pairingStatus.substring(0, 35) : "Discovered Reconclave node"));
  footer("Enter: capabilities   Q/Esc: nodes");
}

void drawNodeCapabilities() {
  header("NODE / CAPABILITIES");
  static const char* const localCapabilities[] = {
      "system.info", "coordination.nodes", "coordination.jobs", "input.keyboard",
      "radio.wifi.scan", "radio.ble.scan", "storage.file.read", "storage.evidence.write",
      "net.discovery.scan"};
  RemoteNode* node = selectedRemoteNode();
  const bool local = selectedNodeId == deviceId;
  const size_t count = local ? sizeof(localCapabilities) / sizeof(localCapabilities[0]) :
      (node ? node->capabilities.size() : 0);
  auto& display = uiCanvas;
  if (count == 0) {
    display.setTextColor(colour(150, 170, 175));
    display.setCursor(8, 42);
    display.print("No capabilities advertised");
    footer("Q/Esc: node management");
    return;
  }
  if (selection >= count) selection = count - 1;
  const size_t first = selection >= 6 ? selection - 5 : 0;
  for (size_t row = 0; row < 6 && first + row < count; ++row) {
    const size_t index = first + row;
    const int y = 28 + static_cast<int>(row) * 15;
    if (index == selection) display.fillRoundRect(4, y - 2, 232, 14, 3, colour(22, 66, 72));
    display.setTextColor(index == selection ? colour(80, 230, 190) : themeTextColour());
    display.setCursor(8, y);
    display.print(index == selection ? "> " : "  ");
    const String capability = local ? String(localCapabilities[index]) : node->capabilities[index];
    const bool enabled = capabilityEnabled(selectedNodeId, capability);
    display.print(enabled ? "[ON] " : "[--] ");
    display.print(capability.substring(0, 29));
  }
  footer("Enter: enable/disable   Q/Esc: node");
}

void drawScout() {
  header("SCOUT / NETWORK DISCOVERY");
  auto& display = uiCanvas;
  const bool scoutFailed = scoutStatus.indexOf("failed") >= 0 ||
      scoutStatus.indexOf("required") >= 0 || scoutStatus.indexOf("unavailable") >= 0 ||
      scoutStatus.indexOf("rejected") >= 0 || scoutStatus.startsWith("Untrusted") ||
      scoutStatus.startsWith("No host") || scoutStatus.indexOf("Unable") >= 0;
  const bool scoutSaved = scoutStatus.startsWith("Saved");
  display.setTextColor(scoutRunning ? colour(255, 190, 70) :
      (scoutFailed ? TFT_MAGENTA : colour(80, 230, 190)));
  display.setCursor(8, 30);
  display.printf("%s", scoutRunning ? "SCANNING" :
      (scoutFailed ? "FAILED" : (scoutSaved ? "SAVED" : "READY")));
  display.setTextColor(colour(150, 170, 175));
  display.setCursor(72, 30);
  display.printf("%u host%s", static_cast<unsigned>(discoveredHosts.size()),
                 discoveredHosts.size() == 1 ? "" : "s");
  display.setTextColor(scoutFailed ? TFT_MAGENTA :
      (scoutRunning ? colour(255, 190, 70) : themeTextColour()));
  display.setCursor(8, 44);
  display.print(scoutStatus.substring(0, 37));
  if (scoutRunning && scoutTotal > 0) {
    display.drawRoundRect(7, 58, 226, 8, 3, colour(35, 118, 112));
    const int width = static_cast<int>((218UL * scoutChecked) / scoutTotal);
    display.fillRoundRect(11, 61, width, 2, 1, colour(80, 230, 190));
  }
  if (discoveredHosts.empty()) {
    display.setTextColor(colour(150, 170, 175));
    display.setCursor(8, 78);
    display.print("No responsive hosts recorded");
  } else {
    if (selection >= discoveredHosts.size()) selection = discoveredHosts.size() - 1;
    const size_t first = selection >= 3 ? selection - 2 : 0;
    for (size_t row = 0; row < 3 && first + row < discoveredHosts.size(); ++row) {
      const size_t index = first + row;
      if (index == selection)
        display.fillRoundRect(4, 70 + static_cast<int>(row) * 14, 232, 13, 3,
                              colour(22, 66, 72));
      display.setTextColor(index == selection ? colour(80, 230, 190) : themeTextColour());
      display.setCursor(8, 73 + static_cast<int>(row) * 14);
      if (scoutExecution == ScoutExecution::Consensus) {
        size_t seen = std::find(localScoutHosts.begin(), localScoutHosts.end(),
                                discoveredHosts[index]) != localScoutHosts.end() ? 1 : 0;
        for (const auto& job : remoteScoutJobs) {
          if (std::find(job.hosts.begin(), job.hosts.end(), discoveredHosts[index]) != job.hosts.end())
            ++seen;
        }
        const size_t providers = remoteScoutJobs.size() + (localScoutAssigned ? 1 : 0);
        display.printf("%c %u/%u  %s", index == selection ? '>' : ' ',
                       static_cast<unsigned>(seen), static_cast<unsigned>(providers),
                       discoveredHosts[index].c_str());
      } else {
        display.printf("%c HOST  %s", index == selection ? '>' : ' ',
                       discoveredHosts[index].c_str());
      }
    }
  }
  footer(scoutRunning ? "Scan active   Q: leave" :
                         "Enter: host   R: scan   Q: back");
}

void drawHostDetail() {
  header("SCOUT / HOST");
  auto& display = uiCanvas;
  display.fillRoundRect(7, 31, 226, 74, 7, colour(11, 30, 40));
  display.drawRoundRect(7, 31, 226, 74, 7, colour(35, 118, 112));
  display.setTextColor(colour(80, 230, 190));
  display.setCursor(16, 42);
  display.print("RESPONSIVE HOST");
  display.setTextColor(themeTextColour());
  display.setTextSize(2);
  display.setCursor(16, 58);
  display.print(selectedHost);
  display.setTextSize(1);
  display.setTextColor(colour(150, 170, 175));
  display.setCursor(16, 84);
  display.print("Enter checks configured TCP services");
  footer("Enter: service scan   Q/Esc: back");
}

void drawPortResults() {
  header("SCOUT / SERVICES");
  auto& display = uiCanvas;
  display.setTextColor(colour(150, 170, 175));
  display.setCursor(8, 29);
  display.print(selectedHost.substring(0, 37));
  display.setTextColor(portRunning ? colour(255, 190, 70) : colour(80, 230, 190));
  display.setCursor(8, 42);
  display.print(portStatus.substring(0, 37));
  if (portRunning && portScan.total() > 0) {
    display.drawRoundRect(7, 55, 226, 8, 3, colour(35, 118, 112));
    const int width = static_cast<int>((218UL * portScan.checked()) / portScan.total());
    display.fillRoundRect(11, 58, width, 2, 1, colour(80, 230, 190));
  }
  if (openPorts.empty()) {
    display.setTextColor(colour(150, 170, 175));
    display.setCursor(8, 75);
    display.print(portRunning ? "Checking services..." : "No open ports in selected scope");
  } else {
    if (selection >= openPorts.size()) selection = openPorts.size() - 1;
    const size_t first = selection >= 2 ? selection - 1 : 0;
    for (size_t row = 0; row < 3 && first + row < openPorts.size(); ++row) {
      const size_t index = first + row;
      if (index == selection)
        display.fillRoundRect(4, 67 + static_cast<int>(row) * 14, 232, 13, 3,
                              colour(22, 66, 72));
      display.setTextColor(index == selection ? colour(80, 230, 190) : themeTextColour());
      display.setCursor(8, 70 + static_cast<int>(row) * 14);
      display.printf("%c TCP %-5u  open", index == selection ? '>' : ' ', openPorts[index]);
    }
  }
  footer(portRunning ? "Checking...   Q: stop/back" : "Q/Esc: host");
}

void drawHome() {
  struct MissionCard { const char* glyph; const char* title; const char* detail; };
  static constexpr MissionCard cards[] = {
      {"NX", "RECONCLAVE", "Coordinate trusted nodes"},
      {"RF", "OBSERVE", "Survey Wi-Fi and BLE"},
      {"IP", "SCOUT", "Discover network assets"},
      {"EV", "EVIDENCE", "Review field records"},
      {"PR", "PROJECTS", "Named engagement scopes"},
      {"FX", "FIELD KIT", "Deck diagnostics and tools"},
      {"IO", "SETTINGS", "Configure this cyberdeck"},
  };
  if (navigationStyle == NavigationStyle::List) {
    static const char* const items[] = {
        "Reconclave", "Observe", "Scout", "Evidence", "Projects", "Field Kit", "Settings"};
    drawMenu("RECONCLAVE / MISSIONS", items, 7, "UP/DOWN: choose   Enter: open");
    return;
  }
  if (selection >= sizeof(cards) / sizeof(cards[0])) selection = 0;
  header("DECK / MISSION CONTROL");
  auto& display = uiCanvas;
  const auto& card = cards[selection];
  // The dashboard keeps live deck state visible without competing with the
  // selected mission. Geometry remains fixed as nodes and storage change.
  display.setTextColor(colour(130, 155, 160));
  display.setCursor(8, 29);
  display.print("LINK");
  display.setTextColor(WiFi.status() == WL_CONNECTED ? colour(80, 230, 190) : colour(255, 190, 70));
  display.setCursor(34, 29);
  display.print(WiFi.status() == WL_CONNECTED ? "UP" : "--");
  display.setTextColor(colour(130, 155, 160));
  display.setCursor(71, 29);
  display.print("NODES");
  display.setTextColor(colour(80, 230, 190));
  display.setCursor(104, 29);
  display.print(remoteNodes.size());
  display.setTextColor(colour(130, 155, 160));
  display.setCursor(137, 29);
  display.print("SD");
  display.setTextColor(sdAvailable ? colour(80, 230, 190) : colour(255, 190, 70));
  display.setCursor(155, 29);
  display.print(sdAvailable ? "RDY" : "---");
  display.setTextColor(colour(130, 155, 160));
  display.setCursor(190, 29);
  display.printf("%02u", static_cast<unsigned>(selection + 1));

  display.fillRoundRect(7, 42, 226, 64, 6, colour(11, 30, 40));
  display.drawRoundRect(7, 42, 226, 64, 6, colour(35, 118, 112));
  display.fillRoundRect(15, 51, 38, 38, 5, colour(22, 66, 72));
  display.setTextColor(colour(80, 230, 190));
  display.setTextSize(2);
  display.setCursor(22, 63);
  display.print(card.glyph);
  display.setTextSize(1);
  display.setCursor(63, 51);
  display.printf("MISSION %02u / 07", static_cast<unsigned>(selection + 1));
  display.setTextColor(themeTextColour());
  display.setTextSize(2);
  display.setCursor(63, 64);
  display.print(card.title);
  display.setTextSize(1);
  display.setTextColor(colour(150, 170, 175));
  display.setCursor(15, 94);
  display.print(card.detail);
  for (size_t index = 0; index < sizeof(cards) / sizeof(cards[0]); ++index) {
    const int x = 91 + static_cast<int>(index) * 10;
    display.drawFastHLine(x, 111, index == selection ? 7 : 4,
                          index == selection ? colour(80, 230, 190) : colour(55, 83, 87));
  }
  footer("ARROWS: mission   Enter: jack in");
}

void drawObserve() {
  static const char* const items[] = {
      "Wi-Fi discovery", "Channel analyser", "BLE discovery",
      "NFC tag reader", "Sub-GHz monitor"};
  drawMenu("OBSERVE SIGNALS", items, sizeof(items) / sizeof(items[0]));

}

void drawCap() {
  const bool isNfc = screen == ScreenState::NfcResults;
  header(isNfc ? "NFC TAG READER" : "SUB-GHZ MONITOR");
  auto& display = uiCanvas;
  display.setTextColor(themeTextColour());
  display.setCursor(8, 28);
  display.print(cap::status().substring(0, 37));
  if (isNfc) {
    const auto& tags = cap::tags();
    if (!tags.empty()) {
      if (selection >= tags.size()) selection = tags.size() - 1;
      const auto& tag = tags[selection];
      display.setCursor(8, 45);
      display.printf("Tag %u/%u  %s", unsigned(selection + 1), unsigned(tags.size()), tag.protocol.c_str());
      display.setCursor(8, 60);
      display.print(tag.uid.substring(0, 37));
      display.setCursor(8, 75);
      display.print(tag.type.substring(0, 37));
      display.setCursor(8, 90);
      display.print(tag.detail.substring(0, 37));
    } else {
      display.setCursor(8, 52);
      display.print("Hold one tag against the cap");
      display.setCursor(8, 70);
      display.print("ISO14443 A/B, FeliCa, ISO15693");
    }
    display.setCursor(8, 106);
    display.print(fieldStatus.isEmpty() ? String("L/R protocol: ") + cap::nfcFilterName() : fieldStatus.substring(0, 37));
    footer(cap::scanningNfc() ? "Enter pause  U/D tags  Q back" : "Enter scan  U/D tags  Q back");
  } else {
    const auto& band = cap::band(cap::selectedBand());
    // Fixed-frequency activity history: newest sample at the right edge.
    // A time trace and heat strip, not a frequency-swept spectrum waterfall.
    display.setCursor(8, 28);
    display.fillRect(0, 26, 240, 12, colour(5, 10, 16));
    const bool radioFailed = cap::status().startsWith("CC1101 unavailable");
    display.printf("< %.2f MHz > %s", band.mhz, radioFailed ? "ERROR" :
        cap::receiving() ? "LIVE" : band.samples ? "PAUSED" : "STOP");
    display.setCursor(8, 41);
    if (band.samples && !radioFailed) display.printf("%s %.0f dBm  peak %.0f", cap::receiving() ? "Now" : "Last", band.rssi, band.peak);
    else display.print(cap::status().substring(0, 37));
    constexpr int left = 34, top = 56, bottom = 96, width = 200;
    display.setTextColor(colour(130, 155, 160));
    for (int step = 0; step < 3; ++step) {
      const int y = top + step * 20;
      display.drawFastHLine(left, y, width, colour(24, 64, 70));
      display.setCursor(2, y - 3);
      display.print(step == 0 ? "-30" : step == 1 ? "-75" : "-120");
    }
    const auto& history = band.history;
    const int offset = width - static_cast<int>(history.size());
    int previousY = bottom;
    for (size_t i = 0; i < history.size(); ++i) {
      const int x = left + offset + static_cast<int>(i);
      const int level = cap::RfHistory::height(history.at(i), bottom - top);
      const int y = bottom - level;
      const uint16_t heat = colour(255 * level / 40, 60 + 150 * level / 40, 170 - 130 * level / 40);
      display.drawFastVLine(x, y, bottom - y + 1, colour(16, 65, 65));
      if (history.connectedToPrevious(i)) display.drawLine(x - 1, previousY, x, y, colour(80, 230, 190));
      else display.drawPixel(x, y, colour(80, 230, 190));
      display.drawFastVLine(x, 99, 5, heat);
      previousY = y;
    }
    display.setCursor(8, 108);
    const int thresholdY = bottom - cap::RfHistory::height(band.threshold, bottom - top);
    for (int x = left; x < left + width; x += 4)
      display.drawFastHLine(x, thresholdY, 2, colour(255, 190, 70));
    if (!fieldStatus.isEmpty()) display.print(fieldStatus.substring(0, 37));
    else if (history.size()) display.printf("U/D %d dBm  above %u%% (%u)", band.threshold,
        history.activityPercent(band.threshold), unsigned(history.size()));
    else display.printf("U/D %d dBm  waiting for samples", band.threshold);
    footer(cap::receiving() ? "Enter pause  L/R band  Q back" : "Enter resume L/R band Q back");
  }
}

bool nfcContentIsUrl() { return nfcAction % 2 == 1; }

void drawNfcViewer() {
  header("NFC / CONTENT VIEWER");
  auto& display = uiCanvas;
  display.setTextColor(themeTextColour());
  display.setCursor(8, 27);
  display.print(cap::status().substring(0, 37));
  display.setCursor(8, 40);
  display.print(nfcReadUid.substring(0, 37));
  for (size_t row = 0; row < 5; ++row) {
    display.setCursor(8, 54 + row * 11);
    display.print(nfcReadText.substring((nfcReadRow + row) * 37, (nfcReadRow + row + 1) * 37));
  }
  display.setTextColor(colour(130, 155, 160));
  display.setCursor(8, 110);
  const size_t lines = (nfcReadText.length() + 36) / 37;
  if (lines) display.printf("Lines %u-%u of %u", unsigned(nfcReadRow + 1),
      unsigned(std::min(nfcReadRow + 5, lines)), unsigned(lines));
  footer("U/D scroll  Enter read  Q back");
}

void drawNfcPresets() {
  std::array<String, 17> labels;
  std::array<const char*, 17> items;
  labels[0] = "Save current content in free slot";
  for (size_t i = 0; i < nfcPresets.size(); ++i)
    labels[i + 1] = String(i + 1) + ": " + (nfcPresets[i].isEmpty() ?
        (nfcPresetOccupied[i] ? "[invalid file]" : "[empty]") :
        String(nfcPresetUrls[i] ? "URL " : "Text ") + nfcPresets[i]);
  for (size_t i = 0; i < items.size(); ++i) {
    labels[i] = labels[i].substring(0, 33);
    items[i] = labels[i].c_str();
  }
  drawMenu("NFC / SD PRESETS", items.data(), items.size());
}

void drawNfcTools() {
  static const char* const items[] = {"Write text to tag", "Write URL to tag",
      "Emulate NFC-A text", "Emulate NFC-A URL", "Emulate FeliCa text", "Emulate FeliCa URL"};
  drawMenu("NFC / WRITE & EMULATE", items, 6);
}

void drawNfcContent() {
  header(nfcAction < 2 ? "NFC / WRITE CONTENT" : "NFC / EMULATE CONTENT");
  auto& display = uiCanvas;
  display.setTextColor(themeTextColour());
  display.setCursor(8, 27);
  display.print(nfcContentIsUrl() ? "URL: http:// or https://" : "Text record (English / UTF-8)");
  for (unsigned row = 0; row < 4; ++row) {
    display.setCursor(8, 41 + row * 12);
    display.print(input.substring(row * 37, (row + 1) * 37));
  }
  display.drawFastHLine(8 + (input.length() % 37) * 6,
      49 + (input.length() / 37) * 12, 5, colour(80, 230, 190));
  display.setCursor(8, 91);
  display.printf("%u/120  %s", unsigned(input.length()), nfcAction < 2 ? "Type 2 NDEF tag" : "New virtual tag");
  display.setCursor(8, 105);
  display.print(fieldStatus.substring(0, 37));
  footer(nfcAction < 2 ? "Enter check  Tab presets  Esc back" : "Enter start  Tab presets  Esc back");
}

void drawNfcWriteConfirm() {
  header("CONFIRM NFC WRITE");
  auto& display = uiCanvas;
  display.setTextColor(colour(255, 190, 70));
  display.setCursor(8, 27);
  display.print("Replace this tag's NDEF content?");
  display.setTextColor(themeTextColour());
  display.setCursor(8, 42);
  display.print(cap::writeTarget().substring(0, 37));
  for (unsigned row = 0; row < 4; ++row) {
    display.setCursor(8, 58 + row * 12);
    display.print(nfcPayload.substring(row * 37, (row + 1) * 37));
  }
  display.setCursor(8, 108);
  display.print("Keep the same tag on the cap");
  footer("Enter: WRITE   Q/Esc: cancel");
}

void drawNfcEmulation() {
  header(nfcAction < 4 ? "NFC-A / VIRTUAL NTAG213" : "FELICA / VIRTUAL NDEF TAG");
  auto& display = uiCanvas;
  display.setTextColor(themeTextColour());
  display.setCursor(8, 29);
  display.print(cap::status().substring(0, 37));
  display.setCursor(8, 47);
  display.print(cap::emulatedIdentifier().substring(0, 37));
  display.setCursor(8, 67);
  display.print(nfcPayload.substring(0, 37));
  display.setCursor(8, 80);
  display.print(nfcPayload.substring(37, 74));
  display.setCursor(8, 104);
  display.print("Fleet servicing paused while active");
  footer("Present cap to reader  Q: stop");
}

void drawWifiChannels() {
  header("OBSERVE / WI-FI CHANNELS");
  auto& display = uiCanvas;
  if (wifiObservations.empty()) {
    display.setTextColor(colour(255, 190, 70));
    display.setCursor(8, 39);
    display.print("No Wi-Fi observations available");
    display.setTextColor(colour(150, 170, 175));
    display.setCursor(8, 57);
    display.print("Use Tab to run a Wi-Fi scan");
    footer("Q/Esc: Observe");
    return;
  }
  int counts[14]{};
  int scores[14]{};
  for (const auto& ap : wifiObservations) {
    if (ap.channel < 1 || ap.channel > 13) continue;
    ++counts[ap.channel];
    scores[ap.channel] += std::max(1, 100 + static_cast<int>(ap.rssi));
  }
  const int choices[] = {1, 6, 11};
  int recommended = choices[0];
  for (int channel : choices)
    if (scores[channel] < scores[recommended]) recommended = channel;
  display.setTextColor(colour(80, 230, 190));
  display.setCursor(8, 29);
  display.printf("RECOMMENDED  CHANNEL %d", recommended);
  for (size_t row = 0; row < 3; ++row) {
    const int channel = choices[row];
    const int y = 49 + static_cast<int>(row) * 21;
    display.setTextColor(channel == recommended ? colour(80, 230, 190) : themeTextColour());
    display.setCursor(8, y);
    display.printf("CH %-2d  %2d AP%s", channel, counts[channel], counts[channel] == 1 ? " " : "s");
    display.drawRoundRect(92, y - 2, 137, 9, 3, colour(35, 118, 112));
    const int width = std::min(129, scores[channel] * 2);
    if (width > 0) display.fillRoundRect(96, y + 1, width, 3, 1,
                                         channel == recommended ? colour(80, 230, 190) : colour(255, 190, 70));
  }
  footer("Q/Esc: Observe");
}

const char* authName(wifi_auth_mode_t auth) {
  return auth == WIFI_AUTH_OPEN ? "open" : "secured";
}

void drawWifiResults() {
  header("WI-FI DISCOVERY");
  auto& display = uiCanvas;
  if (wifiObservations.empty()) {
    display.setTextColor(colour(255, 190, 70));
    display.setCursor(8, 39);
    display.print((fieldStatus.isEmpty() ? String("Use Tab to run a scan") : fieldStatus).substring(0, 37));
  } else {
    if (selection >= wifiObservations.size()) selection = wifiObservations.size() - 1;
    const size_t first = wifiObservations.size() > 5 ? std::min(selection >= 4 ? selection - 3 : size_t(0), wifiObservations.size() - 5) : 0;
    for (size_t row = 0; row < 5 && first + row < wifiObservations.size(); ++row) {
      const size_t index = first + row;
      const auto& ap = wifiObservations[index];
      const int y = 28 + static_cast<int>(row) * 17;
      if (index == selection)
        display.fillRoundRect(3, y - 2, 234, 16, 3, colour(22, 66, 72));
      display.setTextColor(index == selection ? colour(80, 230, 190) : themeTextColour());
      display.setCursor(5, y);
      display.printf("%c%-18s %4ld", index == selection ? '>' : ' ',
                     ap.ssid.substring(0, 18).c_str(), static_cast<long>(ap.rssi));
      display.setTextColor(colour(130, 155, 160));
      display.setCursor(15, y + 9);
      display.printf("ch%ld %s", static_cast<long>(ap.channel), authName(ap.auth));
    }
  }
  if (wifiObservations.size() > 5) {
    const size_t first = std::min(selection >= 4 ? selection - 3 : size_t(0), wifiObservations.size() - 5);
    drawListPosition(first, 5, wifiObservations.size());
  }
  footer(!fieldStatus.isEmpty() ? fieldStatus.c_str() : "Enter: detail   Q/Esc: back");
}

void drawWifiDetail() {
  if (wifiObservations.empty() || selection >= wifiObservations.size()) {
    screen = ScreenState::WifiResults;
    drawWifiResults();
    return;
  }
  const auto& ap = wifiObservations[selection];
  header("WI-FI OBSERVATION");
  auto& display = uiCanvas;
  display.setTextColor(themeTextColour());
  display.setCursor(8, 31);
  display.print(ap.ssid.substring(0, 36));
  display.setCursor(8, 48);
  display.printf("BSSID: %s", ap.bssid.c_str());
  display.setCursor(8, 65);
  display.printf("Channel: %ld", static_cast<long>(ap.channel));
  display.setCursor(8, 82);
  display.printf("Signal: %ld dBm", static_cast<long>(ap.rssi));
  display.setCursor(8, 99);
  display.printf("Security: %s", authName(ap.auth));
  footer(!fieldStatus.isEmpty() ? fieldStatus.c_str() : "Q/Esc: results");
}

void drawBleResults() {
  header("BLE DISCOVERY");
  auto& display = uiCanvas;
  if (bleObservations.empty()) {
    display.setTextColor(colour(255, 190, 70));
    display.setCursor(8, 39);
    display.print((fieldStatus.isEmpty() ? String("Use Tab to run a scan") : fieldStatus).substring(0, 37));
  } else {
    if (selection >= bleObservations.size()) selection = bleObservations.size() - 1;
    const size_t first = bleObservations.size() > 5 ? std::min(selection >= 4 ? selection - 3 : size_t(0), bleObservations.size() - 5) : 0;
    for (size_t row = 0; row < 5 && first + row < bleObservations.size(); ++row) {
      const size_t index = first + row;
      const auto& device = bleObservations[index];
      const int y = 28 + static_cast<int>(row) * 17;
      if (index == selection)
        display.fillRoundRect(3, y - 2, 234, 16, 3, colour(22, 66, 72));
      display.setTextColor(index == selection ? colour(80, 230, 190) : themeTextColour());
      display.setCursor(5, y);
      display.printf("%c%-20s %4d", index == selection ? '>' : ' ',
                     device.name.substring(0, 20).c_str(), device.rssi);
      display.setTextColor(colour(130, 155, 160));
      display.setCursor(15, y + 9);
      display.printf("%s %s", device.address.c_str(),
                     device.connectable ? "connectable" : "broadcast");
    }
  }
  if (bleObservations.size() > 5) {
    const size_t first = std::min(selection >= 4 ? selection - 3 : size_t(0), bleObservations.size() - 5);
    drawListPosition(first, 5, bleObservations.size());
  }
  footer(!fieldStatus.isEmpty() ? fieldStatus.c_str() : "Enter: detail   Q/Esc: back");
}

void drawBleDetail() {
  if (bleObservations.empty() || selection >= bleObservations.size()) {
    screen = ScreenState::BleResults;
    drawBleResults();
    return;
  }
  const auto& device = bleObservations[selection];
  header("BLE OBSERVATION");
  auto& display = uiCanvas;
  display.setTextColor(themeTextColour());
  display.setCursor(8, 31);
  display.print(device.name.substring(0, 36));
  display.setCursor(8, 49);
  display.printf("Address: %s", device.address.c_str());
  display.setCursor(8, 64);
  display.printf("Signal: %d dBm  %s", device.rssi,
                 device.connectable ? "connectable" : "broadcast");
  display.setCursor(8, 79);
  display.printf("Address type: %u  Adv type: %u", device.addressType,
                 device.advertisementType);
  display.setCursor(8, 94);
  display.print((String("Services: ") + String(device.serviceCount) + " " +
      (device.services.isEmpty() ? "none" : device.services)).substring(0, 37));
  display.setCursor(8, 107);
  display.print((device.manufacturer.isEmpty() ? String("Manufacturer: unknown") :
      device.manufacturer + "  " + String(device.payloadLength) + "B").substring(0, 37));
  footer(!fieldStatus.isEmpty() ? fieldStatus.c_str() : "Q/Esc: results");
}

void drawFieldKit() {
  static const char* const items[] = {"Network dashboard", "System diagnostics"};
  drawMenu("FIELD KIT", items, sizeof(items) / sizeof(items[0]));
}

void drawNetworkDashboard() {
  header("FIELD KIT / NETWORK");
  auto& display = uiCanvas;
  if (WiFi.status() != WL_CONNECTED) {
    display.setTextColor(colour(255, 190, 70));
    display.setCursor(8, 38);
    display.print("Wi-Fi is not connected");
    display.setTextColor(colour(150, 170, 175));
    display.setCursor(8, 56);
    display.print("Configure connectivity in Settings");
    footer("Q/Esc: Field Kit");
    return;
  }
  display.setTextColor(themeTextColour());
  display.setCursor(8, 29);
  display.print((String("SSID: ") + WiFi.SSID() + "  " + String(WiFi.RSSI()) + " dBm").substring(0, 37));
  display.setCursor(8, 45);
  display.print("IP:   " + WiFi.localIP().toString());
  display.setCursor(8, 61);
  display.print("GW:   " + WiFi.gatewayIP().toString());
  display.setCursor(8, 77);
  display.print("Mask: " + WiFi.subnetMask().toString());
  display.setCursor(8, 93);
  display.print("DNS:  " + WiFi.dnsIP().toString());
  display.setCursor(8, 107);
  display.print("MAC:  " + WiFi.macAddress());
  footer("Q/Esc: Field Kit");
}

void drawEvidence() {
  header("EVIDENCE");
  auto& display = uiCanvas;
  if (!sdAvailable) {
    display.setTextColor(colour(255, 190, 70));
    display.setCursor(8, 38);
    display.print("microSD unavailable");
    display.setCursor(8, 55);
    display.print("Insert card, then use Tab to refresh");
  } else if (evidenceFiles.empty()) {
    display.setTextColor(colour(150, 170, 175));
    display.setCursor(8, 38);
    display.print("No Reconclave evidence yet");
  } else {
    if (selection >= evidenceFiles.size()) selection = evidenceFiles.size() - 1;
    const size_t first = selection >= 5 ? selection - 4 : 0;
    for (size_t row = 0; row < 6 && first + row < evidenceFiles.size(); ++row) {
      const size_t index = first + row;
      const int y = 28 + static_cast<int>(row) * 15;
      if (index == selection)
        display.fillRoundRect(3, y - 2, 234, 14, 3, colour(22, 66, 72));
      display.setTextColor(index == selection ? colour(80, 230, 190) : themeTextColour());
      display.setCursor(6, y);
      display.printf("%c %-27s %luK", index == selection ? '>' : ' ',
                     evidenceFiles[index].name.substring(0, 27).c_str(),
                     static_cast<unsigned long>(evidenceFiles[index].size / 1024));
    }
  }
  footer(!evidenceFiles.empty() ? "Enter: details   Q/Esc: back" : "Q/Esc: back");
}

String selectedEvidencePath() {
  if (evidenceFiles.empty() || selection >= evidenceFiles.size()) return "";
  String name = evidenceFiles[selection].name;
  return name.startsWith("/") ? name : "/reconclave/evidence/" + name;
}

bool evidencePreviewable(const String& name) {
  String lower = name;
  lower.toLowerCase();
  return lower.endsWith(".txt") || lower.endsWith(".csv") ||
      lower.endsWith(".log") || lower.endsWith(".json") ||
      lower.endsWith(".jsonl") || lower.endsWith(".md");
}

void drawEvidenceDetail() {
  if (evidenceFiles.empty() || selection >= evidenceFiles.size()) {
    screen = ScreenState::Evidence;
    drawEvidence();
    return;
  }
  header("EVIDENCE DETAILS");
  auto& display = uiCanvas;
  const auto& entry = evidenceFiles[selection];
  display.setTextColor(themeTextColour());
  display.setCursor(8, 34);
  display.print(entry.name.substring(0, 36));
  display.setCursor(8, 55);
  display.printf("Size: %llu bytes", static_cast<unsigned long long>(entry.size));
  display.setCursor(8, 76);
  display.printf("Preview: %s", evidencePreviewable(entry.name) ? "available" : "binary/unsupported");
  display.setTextColor(colour(150, 170, 175));
  display.setCursor(8, 96);
  display.print("Facts remain separate from findings");
  footer(evidencePreviewable(entry.name) ? "Enter: preview   Q: back" : "Q: back");
}

void drawEvidencePreview() {
  header("EVIDENCE PREVIEW");
  auto& display = uiCanvas;
  if (previewLines.empty()) {
    display.setTextColor(colour(150, 170, 175));
    display.setCursor(8, 39);
    display.print("(empty or unreadable file)");
  } else {
    for (size_t row = 0; row < 5 && previewLine + row < previewLines.size(); ++row) {
      display.setTextColor(themeTextColour());
      display.setCursor(4, 27 + static_cast<int>(row) * 15);
      display.print(previewLines[previewLine + row].substring(0, 39));
    }
    display.setTextColor(colour(150, 170, 175));
    display.setCursor(178, 105);
    display.printf("%u/%u%s", static_cast<unsigned>(previewLine + 1),
                   static_cast<unsigned>(previewLines.size()), previewTruncated ? "+" : "");
  }
  footer("UP/DOWN: scroll   Q/Esc: details");
}

void drawProjects() {
  header("PROJECTS / ENGAGEMENTS");
  auto& display = uiCanvas;
  if (projects.empty()) {
    display.setTextColor(colour(80, 230, 190));
    display.setCursor(8, 38);
    display.print("NO PROJECTS");
    display.setTextColor(colour(150, 170, 175));
    display.setCursor(8, 58);
    display.print("Tab opens project actions");
    display.setCursor(8, 74);
    display.print("Create a named engagement scope");
    footer("Tab: actions   Q/Esc: missions");
    return;
  }
  if (selection >= projects.size()) selection = projects.size() - 1;
  const size_t first = selection >= 4 ? selection - 3 : 0;
  for (size_t row = 0; row < 4 && first + row < projects.size(); ++row) {
    const size_t index = first + row;
    const int y = 31 + static_cast<int>(row) * 21;
    if (index == selection) display.fillRoundRect(5, y - 3, 230, 19, 4, colour(22, 66, 72));
    display.setTextColor(index == selection ? colour(80, 230, 190) : themeTextColour());
    display.setCursor(8, y);
    display.printf("%c %-20s", index == selection ? '>' : ' ',
                   projects[index].projectId.substring(0, 20).c_str());
    display.setTextColor(colour(130, 155, 160));
    display.setCursor(142, y);
    display.printf("R%u%s", projects[index].revision,
                   projects[index].projectId == activeProjectId ? " ACTIVE" : "");
    display.setCursor(18, y + 10);
    display.print(projects[index].network.substring(0, 32));
  }
  footer("UP/DOWN: project   Enter: inspect");
}

void drawProjectDetail() {
  header("PROJECT / SCOPE");
  auto& display = uiCanvas;
  if (projectSelection >= projects.size()) { drawProjects(); return; }
  const auto& project = projects[projectSelection];
  display.setTextColor(colour(80, 230, 190));
  display.setCursor(8, 32);
  display.print(project.projectId.substring(0, 32));
  display.setTextColor(themeTextColour());
  display.setCursor(8, 52);
  display.printf("DEFAULT SCOPE  REV %u", project.revision);
  display.setCursor(8, 69);
  display.print(project.network.substring(0, 37));
  display.setTextColor(project.projectId == activeProjectId ? colour(80, 230, 190) :
                                                        colour(150, 170, 175));
  display.setCursor(8, 91);
  display.print(project.projectId == activeProjectId ? "ACTIVE SCOUT SCOPE" :
                                                       "Enter to make active");
  footer("Enter: activate   Q/Esc: projects");
}

void drawProvisionProject() {
  header("PROJECTS / NEW");
  auto& display = uiCanvas;
  display.setTextColor(themeTextColour());
  display.setCursor(8, 34);
  display.print("Project identifier");
  display.drawRoundRect(7, 49, 226, 27, 4, colour(35, 118, 112));
  display.setTextColor(colour(80, 230, 190));
  display.setCursor(12, 59);
  display.print(input.substring(input.length() > 32 ? input.length() - 32 : 0));
  display.setTextColor(colour(150, 170, 175));
  display.setCursor(8, 88);
  display.print("Letters, numbers, dot, dash, underscore");
  display.setCursor(8, 103);
  display.print("Default scope uses attached /24");
  footer("Enter: create   Q/Esc: cancel");
}

void drawSystem() {
  header("SYSTEM DIAGNOSTICS");
  auto& display = uiCanvas;
  display.setTextColor(themeTextColour());
  display.setCursor(8, 30);
  display.printf("Firmware: %s", kFirmware);
  display.setCursor(8, 45);
  display.printf("Node: %s", deviceId.substring(deviceId.length() - 12).c_str());
  display.setCursor(8, 60);
  display.printf("Heap: %lu KiB", static_cast<unsigned long>(ESP.getFreeHeap() / 1024));
  display.setCursor(8, 75);
  display.printf("Uptime: %lu s", static_cast<unsigned long>(millis() / 1000));
  display.setCursor(8, 90);
  display.printf("Wi-Fi: %s", WiFi.status() == WL_CONNECTED
                                 ? WiFi.localIP().toString().c_str() : "offline");
  display.setCursor(8, 105);
  display.printf("SD: %s   Trust: %s", sdAvailable ? "ready" : "missing",
                 peerKeyValid ? "paired" : "none");
  footer("Q/Esc: back");
}

void drawSettings() {
  static const char* const items[] = {
      "Connectivity", "Display & interface", "Storage & evidence",
      "Device information", "Trust & pairing"};
  drawMenu("SETTINGS", items, sizeof(items) / sizeof(items[0]));
}

void drawSettingsConnectivity() {
  header("SETTINGS / CONNECTIVITY");
  auto& display = uiCanvas;
  display.setTextColor(colour(80, 230, 190));
  display.setCursor(8, 31);
  display.print(WiFi.status() == WL_CONNECTED ? "CONNECTED" : "OFFLINE");
  display.setTextColor(themeTextColour());
  display.setCursor(8, 49);
  display.print((String("Network: ") + (wifiSsid.isEmpty() ? "not configured" : wifiSsid)).substring(0, 37));
  display.setCursor(8, 66);
  display.print(String("Address: ") + (WiFi.status() == WL_CONNECTED ?
      WiFi.localIP().toString() : "none"));
  display.setTextColor(colour(150, 170, 175));
  display.setCursor(8, 88);
  display.print("Enter to change Wi-Fi credentials");
  footer("Enter: configure   Q/Esc: Settings");
}

void drawSettingsDisplay() {
  header("SETTINGS / DISPLAY");
  auto& display = uiCanvas;
  if (selection >= 9) selection = 0;
  const char* theme = themeLabel();
  const char* navigation = navigationStyle == NavigationStyle::Cards ? "CARDS" : "LIST";
  String timeout = screenTimeoutSeconds == 0 ? "OFF" : String(screenTimeoutSeconds) + "S";
  const char* idle = idleStyle == IdleStyle::Off ? "OFF" :
      (idleStyle == IdleStyle::Radar ? "RADAR" :
       (idleStyle == IdleStyle::Nodes ? "NODES" : "ZETA"));
  const String values[] = {theme, navigation, String(displayBrightness), timeout,
                           idle, "OPEN", searchNodesOnBoot ? "ON" : "OFF",
                           interfaceSounds ? "ON" : "OFF", String(interfaceVolume)};
  static const char* const labels[] = {
      "Theme", "Navigation", "Brightness", "Screen timeout", "Idle animation",
      "Boot screen", "Search nodes boot", "Interface sounds", "Sound volume"};
  const size_t first = selection >= 6 ? selection - 5 : 0;
  for (size_t row = 0; row < 6; ++row) {
    const size_t index = first + row;
    const int y = 28 + static_cast<int>(row) * 15;
    if (index == selection) display.fillRoundRect(4, y - 2, 232, 14, 3, colour(22, 66, 72));
    display.setTextColor(index == selection ? colour(80, 230, 190) : themeTextColour());
    display.setCursor(8, y);
    display.printf("%c %-16s", index == selection ? '>' : ' ', labels[index]);
    display.setTextColor(index == selection ? colour(80, 230, 190) : colour(150, 170, 175));
    display.setCursor(158, y);
    display.print(values[index].substring(0, 12));
  }
  footer("ARROWS: change   Enter: open");
}

const char* bootSequenceLabel() {
  switch (bootSequence) {
    case BootSequence::CipherRain: return "CIPHER RAIN";
    case BootSequence::SignalTrace: return "SIGNAL TRACE";
    case BootSequence::NodeBreach: return "NODE BREACH";
    case BootSequence::PacketStorm: return "PACKET STORM";
    case BootSequence::HexTunnel: return "HEX TUNNEL";
    default: return "ROOT ACCESS";
  }
}

const char* bootSpeedLabel() {
  return bootSpeed == BootSpeed::Slow ? "SLOW" :
      (bootSpeed == BootSpeed::Normal ? "NORMAL" : "FAST");
}

void drawSettingsBoot() {
  header("SETTINGS / BOOT SCREEN");
  if (selection >= 4) selection = 0;
  const String values[] = {bootAnimationEnabled ? "ON" : "OFF", bootSequenceLabel(),
                           bootSpeedLabel(), "RUN"};
  static const char* const labels[] = {"Animation", "Sequence", "Speed", "Preview"};
  auto& display = uiCanvas;
  for (size_t row = 0; row < 4; ++row) {
    const int y = 31 + static_cast<int>(row) * 19;
    if (row == selection) display.fillRoundRect(4, y - 3, 232, 16, 3, colour(22, 66, 72));
    display.setTextColor(row == selection ? colour(80, 230, 190) : themeTextColour());
    display.setCursor(8, y);
    display.printf("%c %-12s", row == selection ? '>' : ' ', labels[row]);
    display.setCursor(126, y);
    display.print(values[row].substring(0, 17));
  }
  footer("ARROWS: change   Q/Esc: back");
}

void drawIdle() {
  auto& display = uiCanvas;
  if (idleStyle == IdleStyle::Off) {
    M5Cardputer.Display.setBrightness(0);
    return;
  }
  display.fillScreen(colour(5, 10, 16));
  display.setTextSize(1);
  display.setTextColor(colour(80, 230, 190));
  display.setCursor(82, 8);
  display.print("RECONCLAVE");
  if (idleStyle == IdleStyle::Radar) {
    const int cx = 120, cy = 75;
    display.drawCircle(cx, cy, 42, colour(35, 118, 112));
    display.drawCircle(cx, cy, 26, colour(24, 64, 70));
    display.drawFastHLine(75, cy, 90, colour(24, 64, 70));
    display.drawFastVLine(cx, 30, 90, colour(24, 64, 70));
    const float angle = static_cast<float>(idlePhase % 360) * 0.0174533f;
    display.drawLine(cx, cy, cx + static_cast<int>(40 * cosf(angle)),
                     cy + static_cast<int>(40 * sinf(angle)), colour(80, 230, 190));
  } else if (idleStyle == IdleStyle::Nodes) {
    const int drift = static_cast<int>((idlePhase / 8) % 9) - 4;
    display.drawLine(58, 83, 120, 47 + drift, colour(35, 118, 112));
    display.drawLine(120, 47 + drift, 183, 85, colour(35, 118, 112));
    display.drawLine(58, 83, 183, 85, colour(24, 64, 70));
    display.fillCircle(58, 83, 6, colour(80, 230, 190));
    display.fillCircle(120, 47 + drift, 7, colour(80, 230, 190));
    display.fillCircle(183, 85, 6, colour(80, 230, 190));
    display.setTextColor(colour(150, 170, 175));
    display.setCursor(86, 108);
    display.print("NODES STANDING BY");
  } else {
    const int bob = static_cast<int>((idlePhase / 12) % 5) - 2;
    display.fillRoundRect(67, 29, 106, 77, 7, colour(9, 28, 39));
    display.drawRoundRect(67, 29, 106, 77, 7, colour(35, 118, 112));
    display.setSwapBytes(true);
    display.pushImage(76, 35 + bob, reconclave_ui::kZetaTitleWidth,
                      reconclave_ui::kZetaTitleHeight,
                      reconclave_ui::kZetaTitlePixels,
                      reconclave_ui::kZetaTransparent);
    display.setSwapBytes(false);
    display.setTextColor(colour(80, 230, 190));
    const uint8_t phase = (idlePhase / 10) % 3;
    display.setCursor(144, 72 - static_cast<int>(phase) * 8);
    display.print(phase == 0 ? "z" : (phase == 1 ? "Z" : "Z Z"));
    display.setTextColor(colour(150, 170, 175));
    display.setCursor(78, 111);
    display.print("DECK STANDING BY");
  }
}

void drawBootAnimation() {
  if (!bootAnimationEnabled) return;
  auto& display = M5Cardputer.Display;
  M5Canvas canvas(&display);
  canvas.setColorDepth(16);
  if (canvas.createSprite(display.width(), display.height()) == nullptr) return;
  const unsigned long duration = bootSpeed == BootSpeed::Slow ? 7500UL :
      (bootSpeed == BootSpeed::Fast ? 2500UL : 5000UL);
  const unsigned long started = millis();
  uint16_t frame = 0;
  while (millis() - started < duration) {
    canvas.fillSprite(colour(5, 10, 16));
    if (bootSequence == BootSequence::CipherRain) {
      for (int column = 0; column < 20; ++column) {
        const int head = (frame * (column % 3 + 2) * 2 + column * 19) % 170 - 18;
        for (int trail = 4; trail >= 0; --trail) {
          const int y = head - trail * 11;
          if (y < 0 || y >= 135) continue;
          canvas.setTextColor(trail == 0 ? colour(80, 230, 190) :
              (trail < 3 ? colour(35, 118, 112) : colour(24, 64, 70)));
          canvas.setCursor(column * 12 + (column & 1) * 2, y);
          canvas.printf("%X", (frame + column * 7 - trail * 3) & 0xF);
        }
      }
      canvas.setTextColor(colour(150, 170, 175));
      canvas.setCursor(7, 121);
      canvas.print("DECRYPTING FIELD STATE...");
    } else if (bootSequence == BootSequence::SignalTrace) {
      for (int x = 0; x < 240; x += 20) canvas.drawFastVLine(x, 19, 97, colour(24, 64, 70));
      for (int y = 19; y < 117; y += 16) canvas.drawFastHLine(0, y, 240, colour(24, 64, 70));
      for (int x = 0; x < 239; ++x) {
        const float wave = sinf((x + frame * 4) * 0.075f) * 18.0f +
            sinf((x - frame * 2) * 0.19f) * 7.0f;
        const float next = sinf((x + 1 + frame * 4) * 0.075f) * 18.0f +
            sinf((x + 1 - frame * 2) * 0.19f) * 7.0f;
        canvas.drawLine(x, 67 + static_cast<int>(wave), x + 1,
                        67 + static_cast<int>(next), colour(80, 230, 190));
      }
      const int sweep = (frame * 3) % 240;
      canvas.drawFastVLine(sweep, 15, 106, colour(255, 190, 70));
      canvas.setTextColor(colour(150, 170, 175));
      canvas.setCursor(7, 121);
      canvas.printf("SIGNAL LOCK %03u", static_cast<unsigned>((frame * 7) % 1000));
    } else if (bootSequence == BootSequence::NodeBreach) {
      const int pulse = (frame * 2) % 54;
      const int nodes[][2] = {{38, 31}, {199, 27}, {47, 105}, {194, 103}, {120, 67}};
      for (size_t index = 0; index < 4; ++index) {
        canvas.drawLine(nodes[index][0], nodes[index][1], 120, 67, colour(35, 118, 112));
        canvas.fillCircle(nodes[index][0], nodes[index][1], 3, colour(80, 230, 190));
      }
      canvas.drawCircle(120, 67, pulse, colour(24, 64, 70));
      canvas.drawCircle(120, 67, std::max(2, pulse - 1), colour(35, 118, 112));
      canvas.fillCircle(120, 67, 7, colour(80, 230, 190));
      canvas.setTextColor(colour(150, 170, 175));
      canvas.setCursor(72, 119);
      canvas.printf("TRUST MAP %02u%%", static_cast<unsigned>((millis() - started) * 100 / duration));
    } else if (bootSequence == BootSequence::PacketStorm) {
      for (int lane = 0; lane < 7; ++lane) {
        canvas.drawFastHLine(0, 15 + lane * 17, 240, colour(24, 64, 70));
        for (int packet = 0; packet < 3; ++packet) {
          const int direction = lane & 1 ? -1 : 1;
          int x = (frame * (lane + 2) * 2 + packet * 83 + lane * 29) % 280 - 20;
          if (direction < 0) x = 220 - x;
          canvas.fillRoundRect(x, 10 + lane * 17, 20 + (packet & 1) * 8, 10, 2,
                               packet == 0 ? colour(80, 230, 190) : colour(35, 118, 112));
        }
      }
      canvas.setTextColor(colour(150, 170, 175));
      canvas.setCursor(7, 124);
      canvas.printf("ROUTING %04u PACKETS", static_cast<unsigned>(frame * 13));
    } else if (bootSequence == BootSequence::HexTunnel) {
      for (int depth = 0; depth < 9; ++depth) {
        const int phase = (frame + depth * 7) % 63;
        const int halfWidth = 8 + phase * 2;
        const int halfHeight = 4 + phase;
        canvas.drawRect(120 - halfWidth, 67 - halfHeight, halfWidth * 2,
                        halfHeight * 2, depth < 3 ? colour(80, 230, 190) : colour(35, 118, 112));
      }
      canvas.setTextColor(colour(150, 170, 175));
      canvas.setCursor(7, 7);
      canvas.printf("0x%04X  MEMORY VECTOR", static_cast<unsigned>(frame * 97));
      canvas.setCursor(72, 123);
      canvas.print("ENTERING SECURE CORE");
    } else {
      const unsigned progress = static_cast<unsigned>((millis() - started) * 100 / duration);
      static const char* const commands[] = {
          "> mount /field", "> verify trust.chain", "> load node.registry",
          "> start secure shell", "> elevate field.agent"};
      canvas.setTextSize(1);
      for (int line = 0; line < 5; ++line) {
        canvas.setTextColor(line <= static_cast<int>(progress / 20) ?
            colour(80, 230, 190) : colour(55, 83, 87));
        canvas.setCursor(11, 14 + line * 18);
        canvas.print(commands[line]);
        if (line < static_cast<int>(progress / 20)) {
          canvas.setCursor(206, 14 + line * 18);
          canvas.print("OK");
        }
      }
      canvas.drawRoundRect(10, 110, 220, 9, 3, colour(35, 118, 112));
      canvas.fillRoundRect(14, 113, static_cast<int>(progress * 212 / 100), 3, 1,
                           colour(80, 230, 190));
      canvas.setTextColor(colour(150, 170, 175));
      canvas.setCursor(87, 124);
      canvas.printf("ACCESS %02u%%", progress);
    }
    canvas.pushSprite(0, 0);
    ++frame;
    delay(33);
  }

  // A distinct, theme-owned identity card closes the sequence. The animation
  // itself stays free of branding so each sequence can tell its own story.
  canvas.fillSprite(colour(5, 10, 16));
  if (uiTheme == UiTheme::NightCity) {
    canvas.fillRect(13, 26, 214, 82, colour(9, 28, 39));
    canvas.drawFastHLine(13, 26, 143, colour(80, 230, 190));
    canvas.drawFastHLine(174, 107, 53, colour(255, 190, 70));
    canvas.fillRect(13, 26, 4, 82, colour(255, 190, 70));
  } else if (uiTheme == UiTheme::Amber) {
    canvas.drawRect(12, 25, 216, 84, colour(35, 118, 112));
    canvas.drawRect(16, 29, 208, 76, colour(24, 64, 70));
    for (int y = 31; y < 104; y += 4) canvas.drawFastHLine(18, y, 204, colour(9, 28, 39));
  } else {
    canvas.fillRoundRect(18, 28, 204, 78, 5, colour(9, 28, 39));
    canvas.drawRoundRect(18, 28, 204, 78, 5, colour(35, 118, 112));
    canvas.drawLine(18, 43, 33, 28, colour(80, 230, 190));
    canvas.drawLine(207, 106, 222, 91, colour(80, 230, 190));
  }
  canvas.setSwapBytes(true);
  canvas.pushImage(22, 39, reconclave_ui::kZetaTitleWidth,
                   reconclave_ui::kZetaTitleHeight,
                   reconclave_ui::kZetaTitlePixels,
                   reconclave_ui::kZetaTransparent);
  canvas.setSwapBytes(false);
  canvas.setTextColor(colour(80, 230, 190));
  canvas.setTextSize(1);
  canvas.setCursor(102, 40);
  canvas.print(uiTheme == UiTheme::Amber ? "> FIELD DECK" : "FIELD DECK // 01");
  canvas.setTextColor(themeTextColour());
  canvas.setTextSize(1);
  canvas.setCursor(102, 58);
  canvas.print("RECONCLAVE");
  canvas.setTextColor(colour(150, 170, 175));
  canvas.setCursor(102, 76);
  canvas.print(uiTheme == UiTheme::NightCity ? "SECURE // ONLINE" :
      (uiTheme == UiTheme::Amber ? "SYSTEM READY_" : "MESH READY"));
  canvas.setTextColor(colour(80, 230, 190));
  canvas.setCursor(102, 91);
  canvas.print("ZETA // OPERATOR");
  canvas.pushSprite(0, 0);
  playUiCue(UiCue::Boot);
  delay(2000);
  canvas.deleteSprite();
}

void drawSettingsStorage() {
  header("SETTINGS / STORAGE");
  auto& display = uiCanvas;
  display.setTextColor(sdAvailable ? colour(80, 230, 190) : colour(255, 190, 70));
  display.setCursor(8, 31);
  display.print(sdAvailable ? "MICROSD READY" : "MICROSD UNAVAILABLE");
  display.setTextColor(themeTextColour());
  display.setCursor(8, 50);
  display.printf("Evidence files: %u", static_cast<unsigned>(evidenceFiles.size()));
  display.setCursor(8, 67);
  display.printf("Card size: %llu MiB", sdAvailable ?
      static_cast<unsigned long long>(SD.cardSize() / (1024ULL * 1024ULL)) : 0ULL);
  display.setTextColor(colour(150, 170, 175));
  display.setCursor(8, 89);
  display.print("Enter remounts and refreshes evidence");
  footer("Enter: refresh   Q/Esc: Settings");
}

void drawSettingsDevice() {
  header("SETTINGS / DEVICE");
  auto& display = uiCanvas;
  if (selection > 1) selection = 0;
  const char* mode = participationMode == ParticipationMode::NodeOnly ? "NODE" :
      participationMode == ParticipationMode::CoordinatorOnly ? "COORDINATOR" : "BOTH";
  static const char* const labels[] = {"Participation", "Diagnostics"};
  for (size_t row = 0; row < 2; ++row) {
    const int y = 34 + static_cast<int>(row) * 31;
    if (selection == row) display.fillRoundRect(4, y - 4, 232, 26, 4, colour(22, 66, 72));
    display.setTextColor(selection == row ? colour(80, 230, 190) : themeTextColour());
    display.setCursor(8, y);
    display.print(selection == row ? '>' : ' ');
    display.setCursor(18, y);
    display.print(labels[row]);
    display.setTextColor(colour(150, 170, 175));
    display.setCursor(18, y + 12);
    display.print(row == 0 ? mode : String(kFirmware) + " / Cardputer ADV");
  }
  footer(selection == 0 ? "LEFT/RIGHT: role   Q/Esc: back" :
                          "Enter: diagnostics   Q/Esc: back");
}

void drawSettingsTrust() {
  header("SETTINGS / TRUST");
  auto& display = uiCanvas;
  if (selection > 2) selection = 0;
  static const char* const labels[] = {"P4 pairing (Grove)", "Execution key", "Evidence key"};
  for (size_t row = 0; row < 3; ++row) {
    const bool active = row == 0 ? peerKeyValid : (row == 1 ? executionKeyValid : evidenceKeyValid);
    const int y = 28 + static_cast<int>(row) * 27;
    if (row == selection) display.fillRoundRect(4, y - 3, 232, 23, 4, colour(22, 66, 72));
    display.setTextColor(row == selection ? colour(80, 230, 190) : themeTextColour());
    display.setCursor(8, y);
    display.print(row == selection ? '>' : ' ');
    display.setCursor(18, y);
    display.print(labels[row]);
    display.setTextColor(active ? colour(80, 230, 190) : colour(150, 170, 175));
    display.setCursor(18, y + 12);
    const String status = row == 0 ? (peerKeyValid ? trustedP4Id : String("not paired; pair over Grove")) :
        (active ? String("configured") : String("not set"));
    display.print(status.substring(0, 33));
  }
  footer(selection == 0
      ? (peerKeyValid ? "Enter: forget   Q/Esc: back" : "Pair via Grove   Q/Esc: back")
      : (selection == 1 ? (executionKeyValid ? "Enter: forget exec key" : "Enter: set exec key") :
         (evidenceKeyValid ? "Enter: forget evidence key" : "Enter: set evidence key")));
}

void drawConfirmForgetTrust() {
  header("CONFIRM / FORGET TRUST");
  auto& display = uiCanvas;
  display.fillRoundRect(7, 31, 226, 75, 7, colour(32, 14, 30));
  display.drawRoundRect(7, 31, 226, 75, 7, TFT_MAGENTA);
  display.setTextColor(TFT_MAGENTA);
  display.setCursor(16, 42);
  display.print("REMOVE SAVED PAIRING?");
  display.setTextColor(themeTextColour());
  display.setCursor(16, 61);
  display.print("Local trust will be erased.");
  display.setCursor(16, 75);
  display.print("Reset P4 trust before pairing again.");
  display.setTextColor(colour(255, 190, 70));
  display.setCursor(16, 95);
  display.print("Enter confirms   Q/Esc cancels");
  footer("Enter: remove   Q/Esc: cancel");
}

RemoteNode* scoutProvider(const String& id) {
  for (auto& node : remoteNodes) {
    if (node.deviceId == id && nodeHasCapability(node, "net.discovery.scan") &&
        capabilityEnabled(node.deviceId, "net.discovery.scan")) return &node;
  }
  return nullptr;
}

String targetLabel() {
  if (scoutTargetId == "auto") return "AUTO";
  if (scoutTargetId == "local") return "LOCAL";
  RemoteNode* node = scoutProvider(scoutTargetId);
  return node ? node->deviceType.substring(0, 12) : "AUTO";
}

const char* portProfileLabel() {
  return portProfile == PortProfile::Web ? "WEB 6" :
      (portProfile == PortProfile::Common ? "COMMON 13" : "EXTENDED 39");
}

const char* scoutExecutionLabel() {
  return scoutExecution == ScoutExecution::Auto ? "AUTO" :
      (scoutExecution == ScoutExecution::Single ? "SINGLE" :
       (scoutExecution == ScoutExecution::Distributed ? "DISTRIBUTED" : "CONSENSUS"));
}

void cycleScoutExecution(bool forward) {
  int value = static_cast<int>(scoutExecution) + (forward ? 1 : 3);
  scoutExecution = static_cast<ScoutExecution>(value % 4);
}

String scoutIntervalLabel() {
  return scoutIntervalMinutes == 0 ? "OFF" : String(scoutIntervalMinutes) + " MIN";
}

void cycleScoutInterval(bool forward) {
  static constexpr uint16_t kPresets[] = {0, 1, 5, 15, 30, 60};
  static constexpr size_t kCount = sizeof(kPresets) / sizeof(kPresets[0]);
  size_t index = 0;
  while (index < kCount && kPresets[index] != scoutIntervalMinutes) ++index;
  if (index >= kCount) index = 0;
  index = (index + (forward ? 1 : kCount - 1)) % kCount;
  scoutIntervalMinutes = kPresets[index];
}

void cycleProjectScope(bool forward) {
  std::vector<String> choices{""};
  for (const auto& project : projects) choices.push_back(project.projectId);
  size_t current = 0;
  for (size_t index = 0; index < choices.size(); ++index)
    if (choices[index] == activeProjectId) current = index;
  current = forward ? (current + 1) % choices.size() :
      (current + choices.size() - 1) % choices.size();
  activeProjectId = choices[current];
  preferences.putString("active_project", activeProjectId);
  scoutBaseline.clear();
  scoutBaselineLoaded = false;
}

void selectedPortList(const uint16_t*& ports, size_t& count) {
  if (portProfile == PortProfile::Web) {
    ports = kWebPorts;
    count = sizeof(kWebPorts) / sizeof(kWebPorts[0]);
  } else if (portProfile == PortProfile::Extended) {
    ports = kExtendedPorts;
    count = sizeof(kExtendedPorts) / sizeof(kExtendedPorts[0]);
  } else {
    ports = kCommonPorts;
    count = sizeof(kCommonPorts) / sizeof(kCommonPorts[0]);
  }
}

size_t contextItemCount() {
  if (contextOrigin == ScreenState::NfcResults) return 6;
  if (contextOrigin == ScreenState::SubGhz) return 4;
  if (contextOrigin == ScreenState::Scout) return 8;
  if (contextOrigin == ScreenState::HostDetail) return 1;
  if (contextOrigin == ScreenState::PortResults) return 2;
  if (contextOrigin == ScreenState::WifiResults || contextOrigin == ScreenState::WifiDetail ||
      contextOrigin == ScreenState::BleResults || contextOrigin == ScreenState::BleDetail ||
      contextOrigin == ScreenState::Reconclave) return 2;
  if (contextOrigin == ScreenState::NodeDetail) return 1;
  if (contextOrigin == ScreenState::Projects) return 2;
  return 1;
}

String contextItemLabel(size_t index) {
  if (contextOrigin == ScreenState::NfcResults) {
    if (index == 0) return "Start / stop NFC scanning";
    if (index == 1) return "Save NFC evidence";
    if (index == 2) return String("Protocol: ") + cap::nfcFilterName();
    if (index == 3) return "Clear NFC history";
    if (index == 4) return "Write & emulate...";
    return "Read text / URL content";
  }
  if (contextOrigin == ScreenState::SubGhz) {
    if (index == 0) return "Start / stop receiver";
    if (index == 1) return "Save RF summary";
    if (index == 2) return "Save RF graph samples";
    return "Clear this band's history";
  }
  if (contextOrigin == ScreenState::Scout) {
    if (index == 0) return String("Scope             ") +
        (activeProjectId.isEmpty() ? "ATTACHED /24" : activeProjectId);
    if (index == 1) return String("Execution         ") + scoutExecutionLabel();
    if (index == 2) return String("Provider          ") + targetLabel();
    if (index == 3) return String("Distribution      ") +
        (distributionStyle == DistributionStyle::Equal ? "EQUAL" : "WEIGHTED");
    if (index == 4) return String("Recurring         ") + scoutIntervalLabel();
    if (index == 5) return String("Job policy        ") +
        (scoutRecurringPolicy == RecurringPolicy::Independent ? "INDEPENDENT" : "CALLBACK");
    if (index == 6) return String("Service scope     ") + portProfileLabel();
    return "Save host evidence";
  }
  if (contextOrigin == ScreenState::HostDetail)
    return String("Service scope     ") + portProfileLabel();
  if (contextOrigin == ScreenState::PortResults) {
    if (index == 0) return String("Service scope     ") + portProfileLabel();
    return "Run service scan again";
  }
  if (contextOrigin == ScreenState::WifiResults || contextOrigin == ScreenState::WifiDetail)
    return index == 0 ? (wifiScanning ? "Stop Wi-Fi scan" : "Run Wi-Fi scan") : "Save Wi-Fi evidence";
  if (contextOrigin == ScreenState::WifiChannels) return "Run Wi-Fi scan";
  if (contextOrigin == ScreenState::BleResults || contextOrigin == ScreenState::BleDetail)
    return index == 0 ? (bleScanning ? "Stop BLE scan" : "Run BLE scan") : "Save BLE evidence";
  if (contextOrigin == ScreenState::Reconclave)
    return index == 0 ? "Refresh devices" : "Pair over Grove";
  if (contextOrigin == ScreenState::NodeDetail) {
    RemoteNode* node = selectedRemoteNode();
    if (selectedNodeId == deviceId) return "Refresh local status";
    return node && nodeHasCapability(*node, "system.info")
        ? "Request system information" : "No management actions available";
  }
  if (contextOrigin == ScreenState::Evidence) return "Refresh microSD evidence";
  if (contextOrigin == ScreenState::Projects)
    return index == 0 ? "Create project" : "Refresh projects";
  if (contextOrigin == ScreenState::System) return "Refresh diagnostics";
  if (contextOrigin == ScreenState::NetworkDashboard) return "Refresh network status";
  if (contextOrigin == ScreenState::Settings) return "Settings use Enter/Left/Right";
  if (contextOrigin == ScreenState::SettingsConnectivity ||
      contextOrigin == ScreenState::SettingsDisplay ||
      contextOrigin == ScreenState::SettingsBoot ||
      contextOrigin == ScreenState::SettingsStorage ||
      contextOrigin == ScreenState::SettingsDevice ||
      contextOrigin == ScreenState::SettingsTrust ||
      contextOrigin == ScreenState::ConfirmForgetTrust)
    return "Settings and actions are shown here";
  if (contextOrigin == ScreenState::Observe) return "Choose an observation source";
  return "Navigation: arrows, Enter, Q";
}

void drawContextMenu() {
  auto& display = uiCanvas;
  const size_t count = contextItemCount();
  if (selection >= count) selection = count - 1;
  // Context is a deck overlay, not another destination in the hierarchy. Keep
  // a sliver of the working screen visible so Tab feels spatial and reversible.
  display.fillRect(14, 4, 226, 131, colour(5, 10, 16));
  display.fillRoundRect(20, 6, 218, 127, 6, colour(9, 28, 39));
  display.drawRoundRect(20, 6, 218, 127, 6, colour(35, 118, 112));
  display.fillRect(20, 6, 4, 127, colour(80, 230, 190));
  display.setTextColor(colour(80, 230, 190));
  display.setCursor(31, 14);
  display.print("CONTEXT // ");
  display.print(count);
  display.fillRoundRect(202, 10, 29, 11, 3, colour(22, 66, 72));
  display.setCursor(206, 12);
  display.print("TAB");
  const size_t visible = count > 5 ? 5 : count;
  const size_t first = selection >= visible ? selection - visible + 1 : 0;
  for (size_t row = 0; row < visible; ++row) {
    const size_t index = first + row;
    const int y = 32 + static_cast<int>(row) * 16;
    if (index == selection) display.fillRoundRect(27, y - 3, 205, 15, 3, colour(22, 66, 72));
    display.setTextColor(index == selection ? colour(80, 230, 190) : themeTextColour());
    display.setCursor(31, y);
    display.printf("%c %s", index == selection ? '>' : ' ', fitUiText(contextItemLabel(index), 185).c_str());
  }
  const bool adjustable = (contextOrigin == ScreenState::Scout && selection < 7) ||
      contextOrigin == ScreenState::HostDetail ||
      (contextOrigin == ScreenState::PortResults && selection == 0);
  RemoteNode* contextNode = contextOrigin == ScreenState::NodeDetail ? selectedRemoteNode() : nullptr;
  const bool nodeActionAvailable = selectedNodeId == deviceId ||
      (contextNode && nodeHasCapability(*contextNode, "system.info"));
  const bool informational = contextOrigin == ScreenState::Home ||
      contextOrigin == ScreenState::Connecting || contextOrigin == ScreenState::Observe ||
      contextOrigin == ScreenState::Settings || contextOrigin == ScreenState::FieldKit ||
      contextOrigin == ScreenState::SettingsConnectivity ||
      contextOrigin == ScreenState::SettingsDisplay ||
      contextOrigin == ScreenState::SettingsBoot ||
      contextOrigin == ScreenState::SettingsStorage ||
      contextOrigin == ScreenState::SettingsDevice ||
      contextOrigin == ScreenState::SettingsTrust ||
      contextOrigin == ScreenState::ConfirmForgetTrust ||
      contextOrigin == ScreenState::NodeCapabilities ||
      contextOrigin == ScreenState::EvidenceDetail ||
      contextOrigin == ScreenState::EvidencePreview ||
      (contextOrigin == ScreenState::NodeDetail && !nodeActionAvailable);
  display.setTextColor(colour(130, 155, 160));
  display.setCursor(31, 116);
  display.print(adjustable ? "LEFT/RIGHT: CHANGE" :
      (informational ? "ESC: CLOSE" : "ENTER: EXECUTE   ESC: CLOSE"));
}

void draw() {
  if (!uiCanvasReady) return;
  if (idleActive) {
    drawIdle();
    uiCanvas.pushSprite(0, 0);
    return;
  }
  if (screen == ScreenState::ProvisionSsid) drawProvision("network name", false);
  else if (screen == ScreenState::ProvisionPassword) drawProvision("password", true);
  else if (screen == ScreenState::Connecting) drawConnecting();
  else if (screen == ScreenState::Home) drawHome();
  else if (screen == ScreenState::Reconclave) drawDashboard();
  else if (screen == ScreenState::NodeDetail) drawNodeDetail();
  else if (screen == ScreenState::NodeCapabilities) drawNodeCapabilities();
  else if (screen == ScreenState::Scout) drawScout();
  else if (screen == ScreenState::HostDetail) drawHostDetail();
  else if (screen == ScreenState::PortResults) drawPortResults();
  else if (screen == ScreenState::NfcResults || screen == ScreenState::SubGhz) drawCap();
  else if (screen == ScreenState::NfcViewer) drawNfcViewer();
  else if (screen == ScreenState::NfcPresets) drawNfcPresets();
  else if (screen == ScreenState::NfcTools) drawNfcTools();
  else if (screen == ScreenState::NfcContent) drawNfcContent();
  else if (screen == ScreenState::NfcWriteConfirm) drawNfcWriteConfirm();
  else if (screen == ScreenState::NfcEmulation) drawNfcEmulation();
  else if (screen == ScreenState::Observe) drawObserve();
  else if (screen == ScreenState::WifiResults) drawWifiResults();
  else if (screen == ScreenState::WifiChannels) drawWifiChannels();
  else if (screen == ScreenState::WifiDetail) drawWifiDetail();
  else if (screen == ScreenState::BleResults) drawBleResults();
  else if (screen == ScreenState::BleDetail) drawBleDetail();
  else if (screen == ScreenState::Evidence) drawEvidence();
  else if (screen == ScreenState::EvidenceDetail) drawEvidenceDetail();
  else if (screen == ScreenState::EvidencePreview) drawEvidencePreview();
  else if (screen == ScreenState::Projects) drawProjects();
  else if (screen == ScreenState::ProjectDetail) drawProjectDetail();
  else if (screen == ScreenState::ProvisionProject) drawProvisionProject();
  else if (screen == ScreenState::FieldKit) drawFieldKit();
  else if (screen == ScreenState::NetworkDashboard) drawNetworkDashboard();
  else if (screen == ScreenState::System) drawSystem();
  else if (screen == ScreenState::Settings) drawSettings();
  else if (screen == ScreenState::SettingsConnectivity) drawSettingsConnectivity();
  else if (screen == ScreenState::SettingsDisplay) drawSettingsDisplay();
  else if (screen == ScreenState::SettingsBoot) drawSettingsBoot();
  else if (screen == ScreenState::SettingsStorage) drawSettingsStorage();
  else if (screen == ScreenState::SettingsDevice) drawSettingsDevice();
  else if (screen == ScreenState::SettingsTrust) drawSettingsTrust();
  else if (screen == ScreenState::ConfirmForgetTrust) drawConfirmForgetTrust();
  else if (screen == ScreenState::ProvisionEvidenceKey)
    drawProvision(provisioningExecutionKey ? "execution key" : "evidence key", true);
  else drawContextMenu();
  uiCanvas.pushSprite(0, 0);
}

String nextMessageId() {
  return deviceId + "-" + String(++messageSequence);
}

void addEnvelope(JsonDocument& document, const char* type, const String& destination) {
  document["proto"] = kProtocol;
  document["type"] = type;
  document["message_id"] = nextMessageId();
  document["source_node"] = deviceId;
  if (!destination.isEmpty()) document["destination_node"] = destination;
  document["timestamp_ms"] = static_cast<uint64_t>(millis()) + 1;
  document["sequence"] = messageSequence;
}

void fillAnnouncement(JsonDocument& document) {
  addEnvelope(document, "announce", "");
  JsonObject payload = document["payload"].to<JsonObject>();
  payload["device_id"] = deviceId;
  payload["device_type"] = "cardputer-adv";
  payload["firmware"] = kFirmware;
  JsonArray roles = payload["roles"].to<JsonArray>();
  roles.add("node");
  if (participationMode != ParticipationMode::NodeOnly) roles.add("coordinator");
  JsonArray capabilities = payload["capabilities"].to<JsonArray>();
  capabilities.add("system.info");
  // Announcements are a remote-call contract, not a list of local UI features.
  // Add capabilities here only when handleMessage implements the corresponding
  // request. Local Wi-Fi/BLE/Scout features remain available from the Cardputer UI.
  if (participationMode != ParticipationMode::CoordinatorOnly && evidenceKeyValid &&
      capabilityEnabled(deviceId, "storage.evidence.write")) {
    capabilities.add("storage.evidence.write");
  }
  if (participationMode != ParticipationMode::CoordinatorOnly && executionKeyValid) {
    capabilities.add("net.discovery.scan");
    capabilities.add("coordination.job.status");
    capabilities.add("coordination.job.cancel");
    if (capabilityEnabled(deviceId, "fleet.ota.apply")) capabilities.add("fleet.ota.apply");
  }
  JsonArray descriptors = payload["capability_descriptors"].to<JsonArray>();
  for (JsonVariant capability : capabilities) {
    const String id = capability.as<String>();
    JsonObject descriptor = descriptors.add<JsonObject>();
    descriptor["id"] = id;
    descriptor["version"] = 1;
    const bool trusted = id == "net.discovery.scan" || id == "storage.evidence.write" ||
        id == "coordination.job.status" || id == "coordination.job.cancel" ||
        id == "fleet.ota.apply";
    descriptor["permission"] = trusted ? "trusted" : "public";
    JsonArray features = descriptor["features"].to<JsonArray>();
    if (id == "net.discovery.scan") {
      features.add("ipv4");
      features.add("range");
      features.add("recurring");
      features.add("independent");
      features.add("callback");
    }
    JsonObject limits = descriptor["limits"].to<JsonObject>();
    limits["weight"] = 1;
    limits["max_concurrency"] = 1;
  }
  JsonObject resources = payload["resources"].to<JsonObject>();
  resources["network_mbps"] = 100;
  resources["persistent_storage"] = sdAvailable;
  resources["storage_free_bytes"] = 0;
  payload["status"] = "ready";
  JsonObject security = payload["security"].to<JsonObject>();
  security["paired"] = true;
  security["boot_nonce"] = nodeBootNonceHex;
  security["mode"] = "provisioned-hmac-sha256-128";
  security["primary_coordinator"] = RC_PROVISIONED_PRIMARY_ID;
  security["coordinator_priority"] = RC_COORDINATOR_PRIORITY;
}

void sendServerJson(JsonDocument& document, int status = 200) {
  String output;
  serializeJson(document, output);
  server.send(status, "application/json", output);
}

void handleAnnounce() {
  JsonDocument document;
  fillAnnouncement(document);
  sendServerJson(document);
}

void addErrorPayload(JsonDocument& response, const String& requestId,
                     const char* code, const char* message, const char* status = "rejected") {
  JsonObject payload = response["payload"].to<JsonObject>();
  payload["request_id"] = requestId;
  payload["status"] = status;
  JsonObject error = payload["error"].to<JsonObject>();
  error["code"] = code;
  error["message"] = message;
}

bool evidenceRecordValid(JsonVariantConst evidence) {
  return evidence.is<JsonObjectConst>() && evidence["job_id"].is<const char*>() &&
      evidence["evidence_id"].is<const char*>() &&
      evidence["source_node"].is<const char*>() && evidence["target"].is<const char*>() &&
      evidence["timestamp_ms"].is<uint64_t>() && evidence["observation"].is<JsonObjectConst>();
}

// Verifies a signed, fresh request for one of the non-benign evidence-network
// capabilities. Replay protection combines this boot session with a bounded
// recent-nonce set rather than sequence numbers, since a sender's sequence counter
// resets to 1 on reboot (see docs/capabilities.md).
bool evidenceRequestAuthenticated(const String& source, const String& destination,
                                  const String& requestId, const char* capability,
                                  JsonVariantConst auth) {
  if (!evidenceKeyValid) return false;
  const String nonce = auth["nonce"] | "";
  const char* tagHex = auth["tag"] | "";
  if (nonce.isEmpty() || tagHex == nullptr || strlen(tagHex) != kTagBytes * 2) return false;
  const String canonical = source + "|" + destination + "|" + requestId + "|" + capability +
      "|" + nodeBootNonceHex + "|" + nonce;
  uint8_t expectedTag[kTagBytes];
  uint8_t suppliedTag[kTagBytes];
  if (!computeEvidenceTag(canonical, expectedTag)) return false;
  if (!hexDecode(suppliedTag, sizeof(suppliedTag), tagHex)) return false;
  if (!constantTimeEqual(expectedTag, suppliedTag, sizeof(expectedTag))) return false;
  return evidenceNonceFresh(nonce);
}

bool executionRequestAuthenticated(const String& source, const String& destination,
                                   const String& requestId, const char* capability,
                                   JsonVariantConst arguments, JsonVariantConst auth,
                                   String& nonceOut) {
  if (!executionKeyValid || source != RC_PROVISIONED_PRIMARY_ID) return false;
  const String nonce = auth["nonce"] | "";
  const char* tagHex = auth["tag"] | "";
  const int priority = auth["coordinator_priority"] | -1;
  const uint32_t leaseMs = auth["lease_ms"] | 0;
  const String suppliedDigest = auth["payload_digest"] | "";
  String payloadDigest;
  if (nonce.isEmpty() || tagHex == nullptr || strlen(tagHex) != kTagBytes * 2 ||
      priority != 100 || leaseMs < 1000 || leaseMs > 60000 ||
      !jsonDigestHex(arguments, payloadDigest) || suppliedDigest != payloadDigest) return false;
  if (std::find(recentExecutionNonces.begin(), recentExecutionNonces.end(), nonce) !=
      recentExecutionNonces.end()) return false;
  const String canonical = source + "|" + destination + "|" + requestId + "|" + capability +
      "|" + nodeBootNonceHex + "|" + payloadDigest + "|" + nonce + "|" +
      String(priority) + "|" + String(leaseMs);
  uint8_t expectedTag[kTagBytes];
  uint8_t suppliedTag[kTagBytes];
  if (!computeExecutionTag(canonical, expectedTag) ||
      !hexDecode(suppliedTag, sizeof(suppliedTag), tagHex) ||
      !constantTimeEqual(expectedTag, suppliedTag, sizeof(expectedTag))) return false;
  recentExecutionNonces.push_back(nonce);
  if (recentExecutionNonces.size() > kRecentNonceCount) recentExecutionNonces.erase(recentExecutionNonces.begin());
  nonceOut = nonce;
  return true;
}

void authenticateExecutionResponse(JsonDocument& response, const String& destination,
                                   const String& requestId, const String& nonce) {
  JsonObject payload = response["payload"];
  const String status = payload["status"] | "rejected";
  String payloadDigest;
  JsonVariantConst responseBody = payload[status == "ok" ? "result" : "error"];
  if (!jsonDigestHex(responseBody, payloadDigest)) return;
  const String canonical = deviceId + "|" + destination + "|" + requestId + "|" + status +
      "|" + nodeBootNonceHex + "|" + payloadDigest + "|" + nonce;
  uint8_t tag[kTagBytes];
  if (!computeExecutionTag(canonical, tag)) return;
  char tagHex[kTagBytes * 2 + 1];
  hexEncode(tagHex, tag, sizeof(tag));
  JsonObject auth = payload["auth"].to<JsonObject>();
  auth["nonce"] = nonce;
  auth["payload_digest"] = payloadDigest;
  auth["tag"] = tagHex;
}

void addRemoteScanResult(JsonObject payload, const String& requestId) {
  payload["request_id"] = requestId;
  payload["status"] = "ok";
  JsonObject result = payload["result"].to<JsonObject>();
  result["job_id"] = remoteNodeJobId;
  const char* status = remoteNodeJobId.isEmpty() ? "idle" :
      remoteHostScan.active() ? "running" : remoteNodeJobCancelled ? "cancelled" :
      remoteNodeJobNextRunMs != 0 ? "waiting" : "complete";
  result["job_status"] = status;
  result["checked"] = remoteHostScan.checked();
  result["total"] = remoteHostScan.total();
  result["recurring"] = remoteNodeJobRecurring;
  result["run_count"] = remoteNodeJobRunCount;
  JsonArray hosts = result["hosts"].to<JsonArray>();
  for (const auto& host : remoteNodeScanHosts) hosts.add(host);
}

bool sha256HexValid(const String& value) {
  if (value.length() != 64) return false;
  for (size_t i = 0; i < value.length(); ++i) {
    if (hexNibble(value[i]) < 0) return false;
  }
  return true;
}

// Tears down whichever live resources an OTA attempt is holding (a flash write in
// progress, a streaming hash context) without disturbing the higher-level bookkeeping
// (armed/token/expected digest) -- callers that still need those for a response, notably
// the success path in OtaUploadHandler::raw, call this alone; otaSessionReset below
// additionally clears that bookkeeping once nothing more needs it.
void otaAbortResources() {
  if (otaSession.mdActive) {
    mbedtls_md_free(&otaSession.md);
    otaSession.mdActive = false;
  }
  if (Update.isRunning()) Update.abort();
}

void otaSessionReset() {
  otaAbortResources();
  otaSession = OtaSession();
}

// Phase 2 of fleet.ota.apply: a raw (non-multipart, non-JSON) POST body registered via
// WebServer's RequestHandler::raw() hook rather than the usual on()/lambda pair, since that
// hook is what lets WebServer stream the body straight through instead of buffering the
// whole request in one std::string first. canRaw/raw() run synchronously inside
// _parseRequest while the body is still arriving; handle() runs once immediately after, to
// actually send the response -- see arduino-esp32's WebServer/src/Parsing.cpp.
class OtaUploadHandler : public RequestHandler {
 public:
  bool canHandle(HTTPMethod method, String uri) override {
    return method == HTTP_POST && uri == kOtaUploadPath;
  }
  bool canRaw(String uri) override { return uri == kOtaUploadPath; }

  void raw(WebServer& srv, String uri, HTTPRaw& raw) override {
    (void)uri;
    switch (raw.status) {
      case RAW_START: {
        otaUploadRejectedAtStart = false;
        otaUploadRejectCode = nullptr;
        const String token = srv.arg("token");
        const uint32_t now = millis();
        const bool expired = !otaSession.armed || (now - otaSession.armedAtMs) > kOtaArmTimeoutMs;
        if (expired || token.isEmpty() || token != otaSession.token) {
          // Deliberately does not touch otaSession: a bogus or replayed request must not be
          // able to cancel a session the real coordinator is still about to upload against.
          otaUploadRejectedAtStart = true;
          otaUploadRejectCode = "OTA_NOT_ARMED";
          return;
        }
        otaSession.armed = false;
        otaSession.inProgress = true;
        break;
      }
      case RAW_WRITE: {
        if (otaUploadRejectedAtStart || !otaSession.inProgress) return;
        if (otaSession.bytesWritten + raw.currentSize > otaSession.maxBytes) {
          otaUploadRejectedAtStart = true;
          otaUploadRejectCode = "OTA_TOO_LARGE";
          otaSessionReset();
          return;
        }
        if (mbedtls_md_update(&otaSession.md, raw.buf, raw.currentSize) != 0 ||
            Update.write(raw.buf, raw.currentSize) != raw.currentSize) {
          otaUploadRejectedAtStart = true;
          otaUploadRejectCode = "OTA_WRITE_FAILED";
          otaSessionReset();
          return;
        }
        otaSession.bytesWritten += raw.currentSize;
        break;
      }
      case RAW_END: {
        if (otaUploadRejectedAtStart || !otaSession.inProgress) return;
        uint8_t digest[32];
        const bool hashed = mbedtls_md_finish(&otaSession.md, digest) == 0;
        char digestHex[65] = {0};
        if (hashed) hexEncode(digestHex, digest, sizeof(digest));
        otaAbortResources(); // frees the (now-finished) md context; leaves Update alone
        if (!hashed) {
          otaUploadRejectCode = "OTA_WRITE_FAILED";
        } else if (String(digestHex) != otaSession.expectedSha256) {
          otaUploadRejectCode = "HASH_MISMATCH";
        } else if (!Update.end(true)) {
          otaUploadRejectCode = "OTA_END_FAILED";
        }
        if (otaUploadRejectCode != nullptr) {
          otaUploadRejectedAtStart = true;
          otaSessionReset();
          return;
        }
        otaSession.committed = true;
        otaSession.resultSha256Hex = digestHex;
        const String canonical = String(deviceId) + "|" + RC_PROVISIONED_PRIMARY_ID + "|" +
            otaSession.token + "|ok|" + otaSession.resultSha256Hex;
        uint8_t tag[kTagBytes];
        if (computeExecutionTag(canonical, tag)) {
          char tagHex[kTagBytes * 2 + 1];
          hexEncode(tagHex, tag, sizeof(tag));
          otaSession.resultTagHex = tagHex;
        }
        break;
      }
      case RAW_ABORTED: {
        // The client disconnected mid-transfer; nobody is left to send a response to, so
        // clean up here rather than deferring to handle() (which never runs for this case).
        otaSessionReset();
        break;
      }
    }
  }

  bool handle(WebServer& srv, HTTPMethod method, String uri) override {
    if (method != HTTP_POST || uri != kOtaUploadPath) return false;
    if (otaSession.committed) {
      const String body = String("{\"status\":\"ok\",\"artifact_sha256\":\"") +
          otaSession.resultSha256Hex + "\",\"tag\":\"" + otaSession.resultTagHex + "\"}";
      srv.send(200, "application/json", body);
      otaSessionReset();
      pendingRebootAtMs = millis() + 800;
      return true;
    }
    const char* code = otaUploadRejectedAtStart && otaUploadRejectCode != nullptr
        ? otaUploadRejectCode : "OTA_NOT_ARMED";
    const int status = strcmp(code, "OTA_TOO_LARGE") == 0 ? 413 :
        strcmp(code, "OTA_NOT_ARMED") == 0 ? 403 : 400;
    srv.send(status, "application/json",
            String("{\"status\":\"rejected\",\"error\":{\"code\":\"") + code +
            "\",\"message\":\"OTA artifact upload failed\"}}");
    return true;
  }
};
OtaUploadHandler otaUploadHandler;

void handleMessage() {
  if (server.arg("plain").length() > reconclave::kMaxPayloadBytes) {
    JsonDocument response;
    addEnvelope(response, "response", "unknown");
    addErrorPayload(response, "invalid-request", "INVALID_REQUEST", "Request exceeds size limit");
    sendServerJson(response);
    return;
  }
  JsonDocument request;
  const DeserializationError error = deserializeJson(request, server.arg("plain"));
  const String source = request["source_node"] | "unknown";
  const String requestId = request["payload"]["request_id"] | "invalid-request";
  const String capability = request["payload"]["capability"] | "";
  JsonDocument response;
  addEnvelope(response, "response", source);
  String executionNonce;
  bool executionAuthenticated = false;
  if (error || String(request["proto"] | "") != kProtocol ||
      String(request["type"] | "") != "request" ||
      String(request["destination_node"] | "") != deviceId) {
    addErrorPayload(response, requestId, "INVALID_REQUEST", "Malformed or misdirected request");
  } else if (capability == "net.discovery.scan" ||
             capability == "coordination.job.status" ||
             capability == "coordination.job.cancel") {
    if (participationMode == ParticipationMode::CoordinatorOnly || !executionKeyValid ||
        !capabilityEnabled(deviceId, capability)) {
      addErrorPayload(response, requestId, "CAPABILITY_UNAVAILABLE", "Capability is not available");
    } else if (!(executionAuthenticated = executionRequestAuthenticated(
                     source, deviceId, requestId, capability.c_str(),
                     request["payload"]["arguments"], request["payload"]["auth"],
                     executionNonce))) {
      addErrorPayload(response, requestId, "UNAUTHENTICATED", "request is not signed or was replayed");
    } else if (capability == "coordination.job.cancel") {
      remoteHostScan.stop();
      remoteNodeJobCancelled = true;
      remoteNodeJobRecurring = false;
      remoteNodeJobNextRunMs = 0;
      remoteNodeCallbackNextAttemptMs = 0;
      saveRemoteTask();
      addRemoteScanResult(response["payload"].to<JsonObject>(), requestId);
    } else if (capability == "coordination.job.status") {
      addRemoteScanResult(response["payload"].to<JsonObject>(), requestId);
    } else if (!evidenceStorageAvailable()) {
      addErrorPayload(response, requestId, "STORAGE_PRESSURE", "Durable evidence storage is unavailable");
    } else {
      JsonObject arguments = request["payload"]["arguments"];
      const char* scopeErrorCode = nullptr;
      const char* scopeErrorMessage = nullptr;
      if (!scopeDelegationValid(arguments, "net.discovery.scan", scopeErrorCode, scopeErrorMessage)) {
        addErrorPayload(response, requestId, scopeErrorCode, scopeErrorMessage);
      } else {
      IPAddress startIp;
      IPAddress endIp;
      const String startText = arguments["start_ip"] | "";
      const String endText = arguments["end_ip"] | "";
      const IPAddress local = WiFi.localIP();
      const IPAddress mask = WiFi.subnetMask();
      const bool validRange = mask == IPAddress(255, 255, 255, 0) &&
          startIp.fromString(startText) && endIp.fromString(endText) &&
          startIp[0] == local[0] && startIp[1] == local[1] && startIp[2] == local[2] &&
          endIp[0] == local[0] && endIp[1] == local[1] && endIp[2] == local[2] &&
          startIp[3] >= 1 && startIp[3] <= endIp[3] && endIp[3] <= 254;
      const String policy = arguments["schedule"]["policy"] | "independent";
      const String callbackEndpoint = arguments["schedule"]["callback_endpoint"] | "";
      const String ownerCoordinator = arguments["schedule"]["owner_coordinator"] | "";
      const bool callbackValid = policy != "callback" ||
          (callbackEndpoint.startsWith("http://") && ownerCoordinator == source);
      if (!validRange || ((policy != "independent" && policy != "callback") &&
                          !arguments["schedule"].isNull()) || !callbackValid) {
        addErrorPayload(response, requestId, "SCOPE_DENIED", "Range or schedule policy is unsupported");
      } else {
        remoteHostScan.stop();
        remoteNodeScanHosts.clear();
        remoteNodeJobId = String(arguments["task_id"] | requestId.c_str());
        remoteNodeJobOwner = source;
        remoteNodeJobCancelled = false;
        remoteNodeFirstHost = startIp[3];
        remoteNodeLastHost = endIp[3];
        remoteNodeJobIntervalMs = arguments["schedule"]["interval_ms"] | 0;
        remoteNodeJobRecurring = remoteNodeJobIntervalMs >= 1000;
        remoteNodeJobCallback = remoteNodeJobRecurring && policy == "callback";
        remoteNodeCallbackEndpoint = remoteNodeJobCallback ? callbackEndpoint : "";
        remoteNodeCallbackFailures = 0;
        remoteNodeCallbackNextAttemptMs = 0;
        remoteNodeJobNextRunMs = 0;
        remoteNodeJobRunCount = 0;
        remoteNodeJobProject = String(arguments["project_id"] | "");
        remoteNodeJobScopeRevision = arguments["scope_revision"] | 0;
        if (!remoteHostScan.startRange(remoteNodeFirstHost,
                                       remoteNodeLastHost - remoteNodeFirstHost + 1)) {
          addErrorPayload(response, requestId, "EXECUTION_FAILED", "Could not start local scanner", "error");
        } else {
          remoteNodeJobWasRunning = true;
          saveRemoteTask();
          addRemoteScanResult(response["payload"].to<JsonObject>(), requestId);
        }
      }
      }
    }
  } else if (capability == "storage.evidence.write") {
    if (!evidenceKeyValid || !capabilityEnabled(deviceId, "storage.evidence.write")) {
      addErrorPayload(response, requestId, "CAPABILITY_UNAVAILABLE", "Capability is not available");
    } else if (!evidenceRequestAuthenticated(source, deviceId, requestId, "storage.evidence.write",
                                             request["payload"]["auth"])) {
      addErrorPayload(response, requestId, "UNAUTHENTICATED", "request is not signed or was replayed");
    } else if (!evidenceRecordValid(request["payload"]["arguments"]["evidence"])) {
      addErrorPayload(response, requestId, "INVALID_REQUEST", "evidence record missing a required field");
    } else {
      String storeError;
      if (writeEvidenceRecord(request["payload"]["arguments"]["evidence"], storeError)) {
        JsonObject payload = response["payload"].to<JsonObject>();
        payload["request_id"] = requestId;
        payload["status"] = "ok";
        JsonObject result = payload["result"].to<JsonObject>();
        result["stored"] = true;
        const String evidenceId = request["payload"]["arguments"]["evidence"]["evidence_id"] | "";
        result["evidence_id"] = evidenceId;
        uint8_t receiptTag[kTagBytes];
        const String receiptCanonical = deviceId + "|" + evidenceId + "|stored";
        if (computeEvidenceTag(receiptCanonical, receiptTag)) {
          char receiptHex[kTagBytes * 2 + 1];
          hexEncode(receiptHex, receiptTag, sizeof(receiptTag));
          result["receipt"].to<JsonObject>()["tag"] = receiptHex;
        }
      } else {
        addErrorPayload(response, requestId, "STORAGE_UNAVAILABLE", storeError.c_str(), "error");
      }
    }
  } else if (capability == "fleet.ota.apply") {
    if (!executionKeyValid || !capabilityEnabled(deviceId, "fleet.ota.apply")) {
      addErrorPayload(response, requestId, "CAPABILITY_UNAVAILABLE", "Capability is not available");
    } else if (!(executionAuthenticated = executionRequestAuthenticated(
                     source, deviceId, requestId, capability.c_str(),
                     request["payload"]["arguments"], request["payload"]["auth"], executionNonce))) {
      addErrorPayload(response, requestId, "UNAUTHENTICATED", "request is not signed or was replayed");
    } else {
      JsonVariantConst arguments = request["payload"]["arguments"];
      JsonVariantConst release = arguments["release"];
      const String releaseDeviceType = release["device_type"] | "";
      const String artifactSha256 = release["artifact_sha256"] | "";
      const String uploadToken = arguments["upload_token"] | "";
      const uint32_t now = millis();
      const bool stale = (otaSession.armed || otaSession.inProgress) &&
          (now - otaSession.armedAtMs) > kOtaArmTimeoutMs;
      if (releaseDeviceType != "cardputer-adv" || !sha256HexValid(artifactSha256) ||
          uploadToken.isEmpty() || uploadToken.length() > 64) {
        addErrorPayload(response, requestId, "RELEASE_INVALID",
                        "release descriptor is missing or does not target this device");
      } else if ((otaSession.armed || otaSession.inProgress) && !stale) {
        addErrorPayload(response, requestId, "OTA_BUSY", "an OTA session is already in progress");
      } else {
        otaSessionReset();
        const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
        mbedtls_md_init(&otaSession.md);
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
          addErrorPayload(response, requestId, "OTA_BEGIN_FAILED",
                          "failed to prepare flash for writing", "error");
        } else if (info == nullptr || mbedtls_md_setup(&otaSession.md, info, 0) != 0 ||
                   mbedtls_md_starts(&otaSession.md) != 0) {
          Update.abort();
          addErrorPayload(response, requestId, "OTA_BEGIN_FAILED",
                          "failed to initialise artifact verification", "error");
        } else {
          otaSession.mdActive = true;
          otaSession.armed = true;
          otaSession.token = uploadToken;
          otaSession.expectedSha256 = artifactSha256;
          otaSession.expectedSha256.toLowerCase();
          otaSession.armedAtMs = now;
          otaSession.bytesWritten = 0;
          otaSession.maxBytes = Update.size();
          JsonObject payload = response["payload"].to<JsonObject>();
          payload["request_id"] = requestId;
          payload["status"] = "ok";
          JsonObject result = payload["result"].to<JsonObject>();
          result["upload_path"] = kOtaUploadPath;
          result["max_bytes"] = otaSession.maxBytes;
        }
      }
    }
  } else if (capability != "system.info") {
    addErrorPayload(response, requestId, "CAPABILITY_UNAVAILABLE", "Capability is not available");
  } else {
    JsonObject payload = response["payload"].to<JsonObject>();
    payload["request_id"] = requestId;
    payload["status"] = "ok";
    JsonObject result = payload["result"].to<JsonObject>();
    result["device_type"] = "cardputer-adv";
    result["firmware"] = kFirmware;
    result["uptime_ms"] = millis();
    result["free_memory_bytes"] = ESP.getFreeHeap();
    result["ip"] = WiFi.localIP().toString();
  }
  if (executionAuthenticated) {
    authenticateExecutionResponse(response, source, requestId, executionNonce);
  }
  sendServerJson(response);
}

void startNodeServices() {
  // Reaching working network services is this device's minimal self-test after an OTA
  // update. If the running bootloader supports app rollback and left this image marked
  // pending-verify, confirm it now; a build that crash-loops before ever getting here relies
  // on the bootloader's own rollback logic instead. Caveat: unlike poe-p4 (an ESP-IDF
  // project whose sdkconfig.defaults now enables CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
  // explicitly), this is a PlatformIO Arduino build against a precompiled framework
  // bootloader -- whether that bootloader was itself built with rollback support isn't
  // something this sketch controls. The call is harmless either way (a no-op error return
  // if the running partition isn't in the pending-verify state).
  static bool otaMarkedValid = false;
  if (!otaMarkedValid) {
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running != nullptr && esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
      esp_ota_mark_app_valid_cancel_rollback();
    }
    otaMarkedValid = true;
  }
  if (!mdnsReady) {
    mdnsReady = MDNS.begin(("reconclave-adv-" + deviceId.substring(deviceId.length() - 6)).c_str());
    if (mdnsReady) {
      MDNS.addService(kService, kTransport, kServerPort);
      MDNS.addServiceTxt(kService, kTransport, "proto", kProtocol);
      MDNS.addServiceTxt(kService, kTransport, "roles", "node,coordinator");
      MDNS.addServiceTxt(kService, kTransport, "device", "cardputer-adv");
      MDNS.addServiceTxt(kService, kTransport, "path", kAnnouncePath);
    }
  }
  if (!serverReady) {
    server.on(kAnnouncePath, HTTP_GET, handleAnnounce);
    server.on(kMessagePath, HTTP_POST, handleMessage);
    server.addHandler(&otaUploadHandler);
    server.onNotFound([] { server.send(404, "application/json", "{\"error\":\"not_found\"}"); });
    server.begin();
    serverReady = true;
  }
}

bool fetchAnnouncement(const IPAddress& address, uint16_t port) {
  WiFiClient client;
  HTTPClient http;
  const String url = "http://" + address.toString() + ":" + String(port) + kAnnouncePath;
  http.setTimeout(2500);
  if (!http.begin(client, url)) return false;
  const int code = http.GET();
  if (code != HTTP_CODE_OK || http.getSize() > kMaxResponseBytes) {
    http.end();
    return false;
  }
  JsonDocument document;
  const DeserializationError parseError = deserializeJson(document, http.getStream());
  http.end();
  if (parseError || String(document["proto"] | "") != kProtocol ||
      String(document["type"] | "") != "announce") return false;
  JsonObject payload = document["payload"];
  const String discoveredId = payload["device_id"] | "";
  const String source = document["source_node"] | "";
  const String discoveredType = payload["device_type"] | "unknown";
  if (discoveredId.isEmpty() || discoveredId != source || discoveredId == deviceId) return false;

  reconclave::NodeAnnouncement announcement;
  announcement.device_id = discoveredId.c_str();
  announcement.device_type = discoveredType.c_str();
  announcement.firmware = String(payload["firmware"] | "").c_str();
  announcement.roles.clear();
  RemoteNode node;
  node.deviceId = discoveredId;
  node.deviceType = discoveredType;
  node.firmware = announcement.firmware.c_str();
  node.ip = address.toString();
  node.port = port;
  node.lastSeenMs = millis();
  for (JsonVariant role : payload["roles"].as<JsonArray>()) {
    const char* value = role.as<const char*>();
    announcement.roles.emplace_back(value);
    if (strcmp(value, "coordinator") == 0) node.coordinator = true;
  }
  for (JsonVariant capability : payload["capabilities"].as<JsonArray>()) {
    const char* value = capability.as<const char*>();
    announcement.capabilities.emplace_back(value);
    node.capabilities.push_back(value);
  }
  for (JsonObject descriptor : payload["capability_descriptors"].as<JsonArray>()) {
    RemoteCapability parsed;
    parsed.id = String(descriptor["id"] | "");
    parsed.permission = String(descriptor["permission"] | "public");
    parsed.weight = constrain(descriptor["limits"]["weight"] | 1, 1, 100);
    parsed.maxConcurrency = constrain(descriptor["limits"]["max_concurrency"] | 1, 1, 1024);
    for (JsonVariant feature : descriptor["features"].as<JsonArray>()) {
      parsed.features.push_back(String(feature.as<const char*>()));
    }
    if (!parsed.id.isEmpty()) node.capabilityDescriptors.push_back(parsed);
  }
  node.networkMbps = constrain(payload["resources"]["network_mbps"] | 0, 0, 65535);
  node.persistentStorage = payload["resources"]["persistent_storage"] | false;
  announcement.status = String(payload["status"] | "").c_str();
  if (!registry.observe(announcement, static_cast<uint64_t>(millis()) + 1)) return false;
  bool updated = false;
  for (auto& known : remoteNodes) {
    if (known.deviceId == discoveredId) {
      known = node;
      updated = true;
      break;
    }
  }
  if (!updated && remoteNodes.size() < reconclave::kMaxDiscoveredNodes) remoteNodes.push_back(node);
  if (nodeHasCapability(node, "storage.evidence.write")) {
    bool collectorUpdated = false;
    for (auto& collector : evidenceCollectors) {
      if (collector.deviceId == node.deviceId) {
        collector = node;
        collectorUpdated = true;
        break;
      }
    }
    if (!collectorUpdated && evidenceCollectors.size() < reconclave::kMaxDiscoveredNodes) {
      evidenceCollectors.push_back(node);
    }
    saveEvidenceCollectors();
  }
  if (discoveredType == "poe-p4") {
    p4 = node;
    const bool remotePaired = payload["security"]["paired"] | false;
    groveBootNonce = String(payload["security"]["boot_nonce"] | "");
    if (peerKeyValid && trustedP4Id == discoveredId && remotePaired &&
        groveBootNonce.length() == 32) pairingStatus = "trusted / secure-ready";
    else if (remotePaired) pairingStatus = "P4 paired to another peer";
    else pairingStatus = "P: pair over Grove";
  }
  return true;
}

void processGroveLine() {
  char* checksumSeparator = strrchr(groveLine, ',');
  if (checksumSeparator == nullptr) return;
  char* checksumEnd = nullptr;
  const uint32_t received = strtoul(checksumSeparator + 1, &checksumEnd, 16);
  if (checksumEnd == checksumSeparator + 1 || *checksumEnd != '\0' ||
      received != crc32(groveLine, checksumSeparator - groveLine)) return;
  *checksumSeparator = '\0';
  if (strncmp(groveLine, "RC1,H,", 6) == 0) {
    char* id = groveLine + 6;
    char* paired = strchr(id, ',');
    if (paired == nullptr) return;
    *paired++ = '\0';
    char* bootNonce = strchr(paired, ',');
    if (bootNonce == nullptr) return;
    *bootNonce++ = '\0';
    groveP4Id = id;
    groveP4Paired = atoi(paired) != 0;
    groveBootNonce = bootNonce;
    lastGroveHeartbeatMs = millis();
    pairingStatus = peerKeyValid && trustedP4Id == groveP4Id
        ? "Grove: trusted peer" : "Grove connected; P to pair";
    if (screen == ScreenState::Reconclave) draw();
  } else if (strncmp(groveLine, "RC1,Q,", 6) == 0 && pairingPending) {
    char* id = groveLine + 6;
    char* fingerprint = strchr(id, ',');
    if (fingerprint == nullptr) return;
    *fingerprint++ = '\0';
    char* accepted = strchr(fingerprint, ',');
    if (accepted == nullptr) return;
    *accepted++ = '\0';
    char expectedFingerprint[9];
    hexEncode(expectedFingerprint, pendingPeerKey, 4);
    if (atoi(accepted) == 1 && strcmp(fingerprint, expectedFingerprint) == 0 &&
        groveP4Id == id) {
      memcpy(peerKey, pendingPeerKey, sizeof(peerKey));
      peerKeyValid = true;
      trustedP4Id = id;
      preferences.putBytes("peer_key", peerKey, sizeof(peerKey));
      preferences.putString("peer_p4", trustedP4Id);
      pairingStatus = "Paired " + String(fingerprint);
      notice = "Trust saved to NVS";
    } else {
      pairingStatus = "Pairing rejected";
    }
    memset(pendingPeerKey, 0, sizeof(pendingPeerKey));
    pairingPending = false;
    draw();
  }
}

void updateGrove() {
  while (groveSerial.available()) {
    const char value = static_cast<char>(groveSerial.read());
    if (value == '\n') {
      groveLine[groveLineLength] = '\0';
      processGroveLine();
      groveLineLength = 0;
    } else if (value != '\r' && groveLineLength + 1 < sizeof(groveLine)) {
      groveLine[groveLineLength++] = value;
    } else if (groveLineLength + 1 >= sizeof(groveLine)) {
      groveLineLength = 0;
    }
  }
}

void beginGrovePairing() {
  if (millis() - lastGroveHeartbeatMs > 3000 || groveP4Id.isEmpty()) {
    pairingStatus = "No Grove P4 detected";
    draw();
    return;
  }
  if (groveP4Paired && (!peerKeyValid || trustedP4Id != groveP4Id)) {
    pairingStatus = "P4 already paired; reset required";
    draw();
    return;
  }
  if (peerKeyValid && trustedP4Id == groveP4Id) {
    memcpy(pendingPeerKey, peerKey, sizeof(peerKey));
  } else {
    esp_fill_random(pendingPeerKey, sizeof(pendingPeerKey));
  }
  char keyHex[kPeerKeyBytes * 2 + 1];
  hexEncode(keyHex, pendingPeerKey, sizeof(pendingPeerKey));
  const String payload = "RC1,P," + deviceId + "," + groveP4Id + "," + keyHex;
  char frame[256];
  snprintf(frame, sizeof(frame), "%s,%08lx\n", payload.c_str(),
           static_cast<unsigned long>(crc32(payload.c_str(), payload.length())));
  groveSerial.print(frame);
  pairingPending = true;
  pairingStatus = "Pairing...";
  draw();
}

void discover() {
  notice = "Discovering _reconclave._tcp";
  draw();
  size_t found = 0;
  const int count = MDNS.queryService(kService, kTransport);
  for (int index = 0; index < count; ++index) {
    if (String(MDNS.txt(index, "proto")) != kProtocol) continue;
    const IPAddress address = MDNS.IP(index);
    const uint16_t port = MDNS.port(index);
    if (address != INADDR_NONE && address != WiFi.localIP() && port != 0 &&
        fetchAnnouncement(address, port)) ++found;
  }
  if (found == 0) {
    const IPAddress fallback = MDNS.queryHost("reconclave-poe-p4", 1500);
    if (fallback != INADDR_NONE && fetchAnnouncement(fallback, kP4FallbackPort)) ++found;
  }
  notice = found ? String(found) + " node(s) discovered" : "No remote nodes found";
  draw();
}

void requestSystemInfo(RemoteNode& provider) {
  if (!nodeHasCapability(provider, "system.info")) {
    notice = "No system.info provider";
    draw();
    return;
  }
  const bool secureP4 = provider.deviceId == p4.deviceId;
  if (secureP4 && (!peerKeyValid || trustedP4Id != provider.deviceId || groveBootNonce.length() != 32)) {
    notice = "Secure pairing required";
    draw();
    return;
  }
  JsonDocument request;
  addEnvelope(request, "request", provider.deviceId);
  JsonObject payload = request["payload"].to<JsonObject>();
  const String requestId = "info-" + String(messageSequence);
  payload["request_id"] = requestId;
  payload["capability"] = "system.info";
  payload["arguments"].to<JsonObject>();
  String requestDigest;
  if (!jsonDigestHex(payload["arguments"], requestDigest)) {
    notice = "Could not hash request";
    draw();
    return;
  }
  char nonceHex[17] = {};
  if (secureP4) {
    uint64_t nonce = 0;
    esp_fill_random(&nonce, sizeof(nonce));
    if (nonce == 0) nonce = 1;
    snprintf(nonceHex, sizeof(nonceHex), "%016llx", static_cast<unsigned long long>(nonce));
    const String canonical = deviceId + "|" + provider.deviceId + "|" + requestId +
        "|system.info|" + groveBootNonce + "|" + requestDigest + "|" + nonceHex + "|" +
        String(RC_COORDINATOR_PRIORITY) + "|" + String(kCoordinatorLeaseMs);
    uint8_t requestTag[kTagBytes];
    if (!computeTag(canonical, requestTag)) {
      notice = "Could not authenticate request";
      draw();
      return;
    }
    char requestTagHex[kTagBytes * 2 + 1];
    hexEncode(requestTagHex, requestTag, sizeof(requestTag));
    JsonObject auth = payload["auth"].to<JsonObject>();
    auth["nonce"] = nonceHex;
    auth["payload_digest"] = requestDigest;
    auth["coordinator_priority"] = RC_COORDINATOR_PRIORITY;
    auth["lease_ms"] = kCoordinatorLeaseMs;
    auth["tag"] = requestTagHex;
  }
  String body;
  serializeJson(request, body);

  WiFiClient client;
  HTTPClient http;
  const String url = "http://" + provider.ip + ":" + String(provider.port) + kMessagePath;
  http.setTimeout(3000);
  if (!http.begin(client, url)) {
    notice = "Could not start request";
    draw();
    return;
  }
  http.addHeader("Content-Type", "application/json");
  const int code = http.POST(body);
  JsonDocument response;
  const DeserializationError parseError = code == HTTP_CODE_OK
      ? deserializeJson(response, http.getStream())
      : DeserializationError::EmptyInput;
  http.end();
  if (code != HTTP_CODE_OK || parseError ||
      String(response["payload"]["request_id"] | "") != requestId) {
    notice = "system.info failed";
  } else {
    const String status = response["payload"]["status"] | "";
    bool authenticated = !secureP4;
    if (secureP4) {
      const String responseNonce = response["payload"]["auth"]["nonce"] | "";
      const String responseTagHex = response["payload"]["auth"]["tag"] | "";
      const String suppliedDigest = response["payload"]["auth"]["payload_digest"] | "";
      String responseDigest;
      JsonVariantConst responseBody = response["payload"][status == "ok" ? "result" : "error"];
      const bool digestValid = jsonDigestHex(responseBody, responseDigest) && suppliedDigest == responseDigest;
      const String responseCanonical = provider.deviceId + "|" + deviceId + "|" +
          requestId + "|" + status + "|" + groveBootNonce + "|" + responseDigest + "|" + responseNonce;
      uint8_t expectedTag[kTagBytes];
      uint8_t suppliedTag[kTagBytes];
      authenticated = digestValid && responseNonce == nonceHex && computeTag(responseCanonical, expectedTag) &&
          hexDecode(suppliedTag, sizeof(suppliedTag), responseTagHex.c_str()) &&
          constantTimeEqual(expectedTag, suppliedTag, sizeof(expectedTag));
    }
    if (!authenticated) {
      notice = "Untrusted response rejected";
    } else if (status == "ok") {
    const uint32_t heap = response["payload"]["result"]["free_memory_bytes"] | 0;
    const uint64_t uptime = response["payload"]["result"]["uptime_ms"] | 0;
    provider.detail = "Heap " + String(heap / 1024) + "K  Up " + String(uptime / 1000) + "s";
    notice = secureP4 ? "Secure system.info received" : "system.info received";
    provider.lastSeenMs = millis();
    } else {
      notice = String(response["payload"]["error"]["code"] | "Request rejected");
    }
  }
  draw();
}

bool callbackCoordinatorReachable() {
  if (!remoteNodeJobCallback || remoteNodeCallbackEndpoint.isEmpty() ||
      remoteNodeJobOwner.isEmpty()) return false;
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(1200);
  String url = remoteNodeCallbackEndpoint;
  if (!url.endsWith(kAnnouncePath)) {
    if (url.endsWith("/")) url.remove(url.length() - 1);
    url += kAnnouncePath;
  }
  if (!http.begin(client, url)) return false;
  const int status = http.GET();
  JsonDocument response;
  const bool parsed = status == HTTP_CODE_OK &&
      deserializeJson(response, http.getStream()) == DeserializationError::Ok;
  http.end();
  return parsed && String(response["payload"]["device_id"] | "") == remoteNodeJobOwner &&
      response["payload"]["roles"].is<JsonArrayConst>();
}

void requestScoutUpdate(RemoteScoutJob& job, bool start) {
  const char* capability = start ? "net.discovery.scan" : "coordination.job.status";
  RemoteNode* provider = scoutProvider(job.nodeId);
  if (provider == nullptr ||
      !nodeHasCapability(*provider, start ? "net.discovery.scan" : "coordination.job.status")) {
    scoutStatus = "Scout provider unavailable";
    job.running = false;
    job.failed = true;
    job.error = scoutStatus;
    return;
  }
  const bool secureP4 = provider->deviceId == p4.deviceId;
  bool trustedCapability = false;
  for (const auto& descriptor : provider->capabilityDescriptors) {
    if (descriptor.id == capability) {
      trustedCapability = descriptor.permission == "trusted";
      break;
    }
  }
  if (secureP4 && (!peerKeyValid || trustedP4Id != p4.deviceId || groveBootNonce.length() != 32)) {
    scoutStatus = "Provider trust required";
    job.running = false;
    job.failed = true;
    job.error = scoutStatus;
    return;
  }
  JsonDocument request;
  addEnvelope(request, "request", provider->deviceId);
  JsonObject payload = request["payload"].to<JsonObject>();
  const String requestId = String(start ? "scan-" : "job-") + String(messageSequence);
  payload["request_id"] = requestId;
  payload["capability"] = capability;
  JsonObject arguments = payload["arguments"].to<JsonObject>();
  if (start) {
    const IPAddress local = WiFi.localIP();
    arguments["network"] = String(local[0]) + "." + String(local[1]) + "." +
        String(local[2]) + ".0/24";
    arguments["start_ip"] = String(local[0]) + "." + String(local[1]) + "." +
        String(local[2]) + "." + String(job.firstHost);
    arguments["end_ip"] = String(local[0]) + "." + String(local[1]) + "." +
        String(local[2]) + "." + String(job.lastHost);
    if (!activeProjectId.isEmpty()) {
      arguments["project_id"] = activeProjectId;
      for (const auto& project : projects) if (project.projectId == activeProjectId) {
        arguments["scope_id"] = "default";
        arguments["scope_revision"] = project.revision;
        break;
      }
    }
    if (scoutIntervalMinutes > 0) {
      JsonObject schedule = arguments["schedule"].to<JsonObject>();
      schedule["interval_ms"] = static_cast<uint32_t>(scoutIntervalMinutes) * 60000UL;
      schedule["after_completion"] = true;
      schedule["policy"] = scoutRecurringPolicy == RecurringPolicy::Independent ?
          "independent" : "callback";
      if (scoutRecurringPolicy == RecurringPolicy::Callback) {
        schedule["owner_coordinator"] = deviceId;
        schedule["callback_endpoint"] = "http://" + WiFi.localIP().toString() +
            ":" + String(kServerPort);
        schedule["max_failures"] = 3;
      }
    }
  }
  String requestDigest;
  if (!jsonDigestHex(arguments, requestDigest)) return;
  char nonceHex[17] = {};
  if (secureP4) {
    uint64_t nonce = 0;
    esp_fill_random(&nonce, sizeof(nonce));
    if (nonce == 0) nonce = 1;
    snprintf(nonceHex, sizeof(nonceHex), "%016llx", static_cast<unsigned long long>(nonce));
    const String canonical = deviceId + "|" + provider->deviceId + "|" + requestId + "|" +
        capability + "|" + groveBootNonce + "|" + requestDigest + "|" + nonceHex + "|" +
        String(RC_COORDINATOR_PRIORITY) + "|" + String(kCoordinatorLeaseMs);
    uint8_t requestTag[kTagBytes];
    if (!computeTag(canonical, requestTag)) return;
    char requestTagHex[kTagBytes * 2 + 1];
    hexEncode(requestTagHex, requestTag, sizeof(requestTag));
    JsonObject auth = payload["auth"].to<JsonObject>();
    auth["nonce"] = nonceHex;
    auth["payload_digest"] = requestDigest;
    auth["coordinator_priority"] = RC_COORDINATOR_PRIORITY;
    auth["lease_ms"] = kCoordinatorLeaseMs;
    auth["tag"] = requestTagHex;
  } else if (trustedCapability) {
    if (!executionKeyValid) {
      scoutStatus = "Provider trust key required";
      job.running = false;
      job.failed = true;
      job.error = scoutStatus;
      return;
    }
    attachExecutionAuth(payload, provider->deviceId, requestId, capability);
  }
  String body;
  serializeJson(request, body);
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(1500);
  const String url = "http://" + provider->ip + ":" + String(provider->port) + kMessagePath;
  if (!http.begin(client, url)) return;
  http.addHeader("Content-Type", "application/json");
  const int code = http.POST(body);
  JsonDocument response;
  const DeserializationError parseError = code == HTTP_CODE_OK
      ? deserializeJson(response, http.getStream()) : DeserializationError::EmptyInput;
  http.end();
  if (code != HTTP_CODE_OK || parseError) {
    scoutStatus = "Remote scout request failed";
    job.running = false;
    job.failed = true;
    job.error = scoutStatus;
    return;
  }
  const String status = response["payload"]["status"] | "";
  bool authenticated = !secureP4;
  if (secureP4) {
    const String responseNonce = response["payload"]["auth"]["nonce"] | "";
    const String responseTagHex = response["payload"]["auth"]["tag"] | "";
    const String suppliedDigest = response["payload"]["auth"]["payload_digest"] | "";
    String responseDigest;
    JsonVariantConst responseBody = response["payload"][status == "ok" ? "result" : "error"];
    const bool digestValid = jsonDigestHex(responseBody, responseDigest) && suppliedDigest == responseDigest;
    const String responseCanonical = provider->deviceId + "|" + deviceId + "|" +
        requestId + "|" + status + "|" + groveBootNonce + "|" + responseDigest + "|" + responseNonce;
    uint8_t expectedTag[kTagBytes];
    uint8_t suppliedTag[kTagBytes];
    authenticated = digestValid && responseNonce == nonceHex && computeTag(responseCanonical, expectedTag) &&
        hexDecode(suppliedTag, sizeof(suppliedTag), responseTagHex.c_str()) &&
        constantTimeEqual(expectedTag, suppliedTag, sizeof(expectedTag));
  }
  if (!authenticated || status != "ok") {
    scoutStatus = authenticated ? "Provider rejected scout job" : "Untrusted job response";
    job.running = false;
    job.failed = true;
    job.error = scoutStatus;
    return;
  }
  JsonObject result = response["payload"]["result"];
  job.checked = result["checked"] | 0;
  job.total = result["total"] | 0;
  const String jobStatus = result["job_status"] | "unknown";
  job.hosts.clear();
  for (JsonVariant host : result["hosts"].as<JsonArray>()) {
    job.hosts.push_back(String(host.as<const char*>()));
  }
  job.running = jobStatus == "running";
  job.failed = jobStatus != "running" && jobStatus != "complete";
  if (job.failed) {
    const String jobError = result["error"] | "";
    job.error = jobError.isEmpty() ? "Remote discovery " + jobStatus : jobError;
  }
  // A recurring provider reports the same job_id across every pass and increments
  // run_count each time one finishes; ship that pass's results as soon as we see it,
  // rather than waiting for the job to stop (it may run for hours).
  const uint16_t runCount = result["run_count"] | 0;
  if (runCount > job.runCount) {
    job.runCount = runCount;
    distributeScoutEvidence(job.hosts, job.nodeId);
  }
}

// Idempotent per docs/capabilities.md: safe to call even if the provider's job
// already finished or was never recurring.
// Attaches a fresh signed auth block for one of the evidence-network capabilities.
// Requires evidenceKeyValid (set on Settings > Trust); no-ops otherwise, so a receiver
// that also has no key configured simply won't have advertised the capability, and one
// that does will correctly reject the resulting unsigned request.
void attachEvidenceAuth(JsonObject payload, const String& destination, const String& requestId,
                        const char* capability) {
  if (!evidenceKeyValid) return;
  uint8_t nonce[8];
  esp_fill_random(nonce, sizeof(nonce));
  char nonceHex[17];
  hexEncode(nonceHex, nonce, sizeof(nonce));
  const String canonical = deviceId + "|" + destination + "|" + requestId + "|" + capability + "|" + nonceHex;
  uint8_t tag[kTagBytes];
  if (!computeEvidenceTag(canonical, tag)) return;
  char tagHex[kTagBytes * 2 + 1];
  hexEncode(tagHex, tag, sizeof(tag));
  JsonObject auth = payload["auth"].to<JsonObject>();
  auth["nonce"] = nonceHex;
  auth["tag"] = tagHex;
}

void attachExecutionAuth(JsonObject payload, const String& destination, const String& requestId,
                         const char* capability) {
  if (!executionKeyValid) return;
  uint8_t nonce[8];
  esp_fill_random(nonce, sizeof(nonce));
  char nonceHex[17];
  hexEncode(nonceHex, nonce, sizeof(nonce));
  const String canonical = deviceId + "|" + destination + "|" + requestId + "|" + capability + "|" + nonceHex;
  uint8_t tag[kTagBytes];
  if (!computeExecutionTag(canonical, tag)) return;
  char tagHex[kTagBytes * 2 + 1];
  hexEncode(tagHex, tag, sizeof(tag));
  JsonObject auth = payload["auth"].to<JsonObject>();
  auth["nonce"] = nonceHex;
  auth["tag"] = tagHex;
}

void requestScoutCancel(const RemoteScoutJob& job) {
  if (!executionKeyValid) return;
  RemoteNode* provider = scoutProvider(job.nodeId);
  if (provider == nullptr) return;
  JsonDocument request;
  addEnvelope(request, "request", provider->deviceId);
  JsonObject payload = request["payload"].to<JsonObject>();
  const String requestId = String("cancel-") + String(messageSequence);
  payload["request_id"] = requestId;
  payload["capability"] = "coordination.job.cancel";
  payload["arguments"].to<JsonObject>();
  attachExecutionAuth(payload, provider->deviceId, requestId, "coordination.job.cancel");
  String body;
  serializeJson(request, body);
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(1500);
  const String url = "http://" + provider->ip + ":" + String(provider->port) + kMessagePath;
  if (!http.begin(client, url)) return;
  http.addHeader("Content-Type", "application/json");
  http.POST(body);
  http.end();
}

uint8_t scoutProviderWeight(const RemoteNode* node) {
  if (node == nullptr) return 1;
  for (const auto& descriptor : node->capabilityDescriptors) {
    if (descriptor.id == "net.discovery.scan") return descriptor.weight;
  }
  return 1;
}

bool scoutProviderUsable(const RemoteNode& node) {
  if (!nodeHasCapability(node, "net.discovery.scan") ||
      !capabilityEnabled(node.deviceId, "net.discovery.scan")) return false;
  if (scoutIntervalMinutes > 0 &&
      (!nodeCapabilityHasFeature(node, "net.discovery.scan", "recurring") ||
       !nodeCapabilityHasFeature(node, "net.discovery.scan",
           scoutRecurringPolicy == RecurringPolicy::Independent ? "independent" : "callback")))
    return false;
  if (node.deviceId != p4.deviceId) return true;
  return peerKeyValid && trustedP4Id == p4.deviceId && groveBootNonce.length() == 32;
}

// Pushes one evidence record to a known Evidence Collector node. Collectors only ever
// advertise storage.evidence.write once they have an evidence key configured, and this
// call requires one too (see attachEvidenceAuth) to sign the request.
bool sendEvidenceToNode(const RemoteNode& node, JsonVariantConst evidence) {
  if (!evidenceKeyValid) return false;
  JsonDocument request;
  addEnvelope(request, "request", node.deviceId);
  JsonObject payload = request["payload"].to<JsonObject>();
  const String requestId = String("evidence-") + String(messageSequence);
  payload["request_id"] = requestId;
  payload["capability"] = "storage.evidence.write";
  payload["arguments"].to<JsonObject>()["evidence"].set(evidence);
  attachEvidenceAuth(payload, node.deviceId, requestId, "storage.evidence.write");
  String body;
  serializeJson(request, body);
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(1500);
  const String url = "http://" + node.ip + ":" + String(node.port) + kMessagePath;
  if (!http.begin(client, url)) return false;
  http.addHeader("Content-Type", "application/json");
  const int code = http.POST(body);
  JsonDocument response;
  const DeserializationError parseError = code == HTTP_CODE_OK
      ? deserializeJson(response, http.getStream()) : DeserializationError::EmptyInput;
  http.end();
  if (code != HTTP_CODE_OK || parseError ||
      String(response["payload"]["request_id"] | "") != requestId ||
      String(response["payload"]["status"] | "") != "ok" ||
      !(response["payload"]["result"]["stored"] | false)) return false;
  const String evidenceId = response["payload"]["result"]["evidence_id"] | "";
  const String receiptHex = response["payload"]["result"]["receipt"]["tag"] | "";
  if (evidenceId.isEmpty() || receiptHex.length() != kTagBytes * 2) return false;
  uint8_t expected[kTagBytes];
  uint8_t supplied[kTagBytes];
  const String canonical = node.deviceId + "|" + evidenceId + "|stored";
  return computeEvidenceTag(canonical, expected) &&
      hexDecode(supplied, sizeof(supplied), receiptHex.c_str()) &&
      constantTimeEqual(expected, supplied, sizeof(expected));
}

bool queueEvidenceForNode(const RemoteNode& node, JsonVariantConst evidence) {
  JsonDocument queued;
  queued["destination_node"] = node.deviceId;
  queued["evidence"].set(evidence);
  if (!sdAvailable) mountEvidence();
  if (!sdAvailable) {
    String encoded;
    serializeJson(queued, encoded);
    if (encoded.length() > 3000) return false;
    for (uint8_t slot = 0; slot < 4; ++slot) {
      const String key = "out" + String(slot);
      if (preferences.getString(key.c_str(), "").isEmpty()) {
        const bool stored = preferences.putString(key.c_str(), encoded) == encoded.length();
        if (stored) ++pendingEvidenceCount;
        return stored;
      }
    }
    return false;
  }
  SD.mkdir("/reconclave/outbox");
  String evidenceId = evidence["evidence_id"] | "";
  if (evidenceId.isEmpty()) evidenceId = String("legacy-") + String(millis());
  String safeNode = node.deviceId;
  safeNode.replace("/", "_");
  const String path = "/reconclave/outbox/" + evidenceId + "-" + safeNode + ".json";
  if (SD.exists(path)) return true;
  File file = SD.open(path, FILE_WRITE);
  if (!file) return false;
  const bool written = serializeJson(queued, file) > 0;
  file.close();
  if (written) ++pendingEvidenceCount;
  return written;
}

void retryEvidenceOutbox() {
  if (!evidenceKeyValid || WiFi.status() != WL_CONNECTED) return;
  for (uint8_t slot = 0; slot < 4; ++slot) {
    const String key = "out" + String(slot);
    const String encoded = preferences.getString(key.c_str(), "");
    if (encoded.isEmpty()) continue;
    JsonDocument queued;
    if (deserializeJson(queued, encoded) != DeserializationError::Ok) continue;
    const String destination = queued["destination_node"] | "";
    for (auto& node : evidenceCollectors) {
      if (node.deviceId == destination && sendEvidenceToNode(node, queued["evidence"])) {
        preferences.remove(key.c_str());
        if (pendingEvidenceCount > 0) --pendingEvidenceCount;
        break;
      }
    }
  }
  if (!sdAvailable) return;
  File directory = SD.open("/reconclave/outbox");
  if (!directory || !directory.isDirectory()) return;
  File file = directory.openNextFile();
  uint8_t attempted = 0;
  while (file && attempted < 3) {
    const String path = String(file.path());
    JsonDocument queued;
    const bool parsed = deserializeJson(queued, file) == DeserializationError::Ok;
    file.close();
    if (parsed) {
      const String destination = queued["destination_node"] | "";
      RemoteNode* provider = nullptr;
      for (auto& node : evidenceCollectors) {
        if (node.deviceId == destination) { provider = &node; break; }
      }
      if (provider && sendEvidenceToNode(*provider, queued["evidence"])) {
        SD.remove(path);
        if (pendingEvidenceCount > 0) --pendingEvidenceCount;
      }
    }
    ++attempted;
    file = directory.openNextFile();
  }
  directory.close();
}

bool evidenceStorageAvailable() {
  if (!sdAvailable) mountEvidence();
  if (!sdAvailable) return false;
  const uint64_t capacity = SD.cardSize();
  const uint64_t used = SD.usedBytes();
  constexpr uint64_t kEvidenceReserveBytes = 1024ULL * 1024ULL;
  return capacity > used && capacity - used >= kEvidenceReserveBytes;
}

// Distributes a copy of a Scout run's results to every enabled Evidence Collector,
// including this Cardputer itself when it is one. For a one-shot job this covers the
// whole result set once at completion; for a recurring job, callers invoke this once
// per run with that run's own hosts, so each pass is shipped as it finishes.
void distributeScoutEvidence(const std::vector<String>& hosts, const String& observer) {
  if (hosts.empty()) return;
  const String jobId = "scout-" + String(millis());
  for (const String& host : hosts) {
    JsonDocument evidenceDoc;
    JsonObject evidence = evidenceDoc.to<JsonObject>();
    evidence["job_id"] = jobId;
    evidence["evidence_id"] = deviceId + "-" + jobId + "-" + host;
    evidence["source_node"] = observer;
    evidence["target"] = host;
    evidence["timestamp_ms"] = static_cast<uint64_t>(millis()) + 1;
    evidence["method"] = "net.discovery.scan";
    if (!activeProjectId.isEmpty()) {
      evidence["project_id"] = activeProjectId;
      for (const auto& project : projects) if (project.projectId == activeProjectId) {
        evidence["scope_id"] = "default";
        evidence["scope_revision"] = project.revision;
        break;
      }
    }
    evidence["observation"].to<JsonObject>()["responsive"] = true;
    if (capabilityEnabled(deviceId, "storage.evidence.write")) {
      String storeError;
      writeEvidenceRecord(evidence, storeError);
    }
    for (const auto& node : evidenceCollectors) {
      if (nodeHasCapability(node, "storage.evidence.write") &&
          capabilityEnabled(node.deviceId, "storage.evidence.write")) {
        if (evidenceKeyValid && !sendEvidenceToNode(node, evidence)) {
          queueEvidenceForNode(node, evidence);
        }
        else scoutEvidenceKeyMissing = true;
      }
    }
  }
}

// Deterministic (non-AI) change detection over a completed run's host set, compared
// to the previous completed run. Scoped to jobs with one clean "run finished" edge:
// any one-shot Scout job, or a local-only recurring one - see docs/capabilities.md for
// why multi-provider recurring isn't covered yet. Appeared/vanished hosts are recorded
// as evidence (not just shown on screen) since the point of an unattended recurring
// scan is that nobody may be watching when the change happens.
void detectAndRecordChanges(const std::vector<String>& hosts, const String& observer) {
  if (!scoutBaselineLoaded) {
    loadScoutBaseline();
    scoutBaselineLoaded = true;
  }
  scoutLastAppeared = 0;
  scoutLastVanished = 0;
  if (scoutBaseline.empty()) {
    scoutBaseline = hosts;
    saveScoutBaseline();
    return;
  }
  std::vector<String> appeared;
  std::vector<String> vanished;
  for (const String& host : hosts) {
    if (std::find(scoutBaseline.begin(), scoutBaseline.end(), host) == scoutBaseline.end()) {
      appeared.push_back(host);
    }
  }
  for (const String& host : scoutBaseline) {
    if (std::find(hosts.begin(), hosts.end(), host) == hosts.end()) vanished.push_back(host);
  }
  const String jobId = "change-" + String(millis());
  const auto emit = [&](const String& host, const char* change) {
    JsonDocument evidenceDoc;
    JsonObject evidence = evidenceDoc.to<JsonObject>();
    evidence["job_id"] = jobId;
    evidence["evidence_id"] = deviceId + "-" + jobId + "-" + host + "-" + change;
    evidence["source_node"] = observer;
    evidence["target"] = host;
    evidence["timestamp_ms"] = static_cast<uint64_t>(millis()) + 1;
    evidence["method"] = "coordination.change.detect";
    if (!activeProjectId.isEmpty()) evidence["project_id"] = activeProjectId;
    evidence["observation"].to<JsonObject>()["change"] = change;
    if (capabilityEnabled(deviceId, "storage.evidence.write")) {
      String storeError;
      writeEvidenceRecord(evidence, storeError);
    }
    for (const auto& node : evidenceCollectors) {
      if (nodeHasCapability(node, "storage.evidence.write") &&
          capabilityEnabled(node.deviceId, "storage.evidence.write")) {
        if (evidenceKeyValid && !sendEvidenceToNode(node, evidence)) {
          queueEvidenceForNode(node, evidence);
        }
        else scoutEvidenceKeyMissing = true;
      }
    }
  };
  for (const String& host : appeared) emit(host, "appeared");
  for (const String& host : vanished) emit(host, "vanished");
  scoutLastAppeared = appeared.size();
  scoutLastVanished = vanished.size();
  scoutBaseline = hosts;
  saveScoutBaseline();
}

void mergeScoutProgress() {
  uint32_t checked = localScoutAssigned ? localHostScan.checked() : 0;
  uint32_t total = localScoutAssigned ? localHostScan.total() : 0;
  bool running = localScoutAssigned && localHostScan.active();
  size_t failures = 0;
  for (const auto& job : remoteScoutJobs) {
    checked += job.checked;
    total += job.total ? job.total : job.lastHost - job.firstHost + 1;
    running = running || job.running;
    if (job.failed) ++failures;
    for (const auto& host : job.hosts) {
      if (std::find(discoveredHosts.begin(), discoveredHosts.end(), host) == discoveredHosts.end())
        discoveredHosts.push_back(host);
    }
  }
  std::sort(discoveredHosts.begin(), discoveredHosts.end(), [](const String& left, const String& right) {
    IPAddress a, b;
    a.fromString(left); b.fromString(right);
    return static_cast<uint32_t>(a) < static_cast<uint32_t>(b);
  });
  scoutChecked = checked;
  scoutTotal = total;
  scoutRunning = running;
  if (!running && !scoutEvidenceSent && scoutIntervalMinutes == 0 &&
      (localScoutAssigned || !remoteScoutJobs.empty())) {
    // Recurring jobs ship each run's evidence as it finishes (see requestScoutUpdate
    // and the local reschedule in loop()); this path is only for one-shot jobs.
    const String observer = scoutRemoteNodeId.isEmpty() ? deviceId : scoutRemoteNodeId;
    distributeScoutEvidence(discoveredHosts, observer);
    detectAndRecordChanges(discoveredHosts, observer);
    scoutEvidenceSent = true;
  }
  String statusSuffix;
  if (scoutLastAppeared > 0 || scoutLastVanished > 0) {
    statusSuffix = " (+" + String(scoutLastAppeared) + "/-" + String(scoutLastVanished) + ")";
  }
  if (scoutEvidenceKeyMissing) statusSuffix += " NOKEY";
  if (running) {
    scoutStatus = String(remoteScoutJobs.size() + (localScoutAssigned ? 1 : 0)) +
        " providers " + String(checked) + "/" + String(total);
  } else if (failures > 0) {
    scoutStatus = String(discoveredHosts.size()) + " found; " + String(failures) + " provider failed";
  } else {
    scoutStatus = String(checked) + "/" + String(total) + " | " +
        String(discoveredHosts.size()) + " found" + statusSuffix;
  }
}

void startScoutJob() {
  if (!evidenceStorageAvailable()) {
    scoutRunning = false;
    scoutStatus = "Paused: evidence storage low/missing";
    draw();
    return;
  }
  if (!activeProjectId.isEmpty()) {
    JsonDocument auditDetail;
    auditDetail["capability"] = "net.discovery.scan";
    auditDetail["executor"] = scoutExecutionLabel();
    auditDetail["provider"] = targetLabel();
    auditDetail["interval_minutes"] = scoutIntervalMinutes;
    auditDetail["policy"] = scoutRecurringPolicy == RecurringPolicy::Independent ?
        "independent" : "callback";
    appendProjectAudit("task.started", auditDetail.as<JsonVariantConst>());
  }
  discoveredHosts.clear();
  localScoutHosts.clear();
  remoteScoutJobs.clear();
  localScoutAssigned = false;
  scoutChecked = 0;
  scoutTotal = 0;
  scoutEvidenceSent = false;
  scoutRunCount = 0;
  localScoutNextRunMs = 0;
  scoutLastAppeared = 0;
  scoutLastVanished = 0;
  scoutEvidenceKeyMissing = false;

  std::vector<RemoteNode*> remotes;
  if (participationMode != ParticipationMode::NodeOnly) {
    for (auto& node : remoteNodes) if (scoutProviderUsable(node)) remotes.push_back(&node);
  }
  std::sort(remotes.begin(), remotes.end(), [](const RemoteNode* left, const RemoteNode* right) {
    return scoutProviderWeight(left) > scoutProviderWeight(right);
  });

  const bool consensus = scoutExecution == ScoutExecution::Consensus;
  bool distributed = scoutExecution == ScoutExecution::Distributed || consensus ||
      (scoutExecution == ScoutExecution::Auto && !remotes.empty());
  if (scoutExecution == ScoutExecution::Single) distributed = false;

  if (!distributed) {
    RemoteNode* provider = nullptr;
    if (scoutTargetId != "local" && scoutTargetId != "auto") provider = scoutProvider(scoutTargetId);
    else if (scoutTargetId == "auto" && !remotes.empty()) provider = remotes.front();
    if (provider != nullptr && scoutProviderUsable(*provider)) {
      RemoteScoutJob job;
      job.nodeId = provider->deviceId;
      job.firstHost = 1;
      job.lastHost = 254;
      remoteScoutJobs.push_back(job);
      requestScoutUpdate(remoteScoutJobs.back(), true);
      scoutExecutor = provider->deviceType;
      scoutRemoteNodeId = provider->deviceId;
    } else {
      localScoutFirstHost = 1;
      localScoutLastHost = 254;
      localScoutAssigned = capabilityEnabled(deviceId, "net.discovery.scan") &&
          localHostScan.startRange(1, 254);
      scoutExecutor = "cardputer";
      scoutRemoteNodeId = "";
    }
  } else {
    struct Assignment { RemoteNode* node; uint8_t weight; };
    std::vector<Assignment> assignments;
    if (capabilityEnabled(deviceId, "net.discovery.scan")) assignments.push_back({nullptr, 1});
    for (auto* node : remotes) assignments.push_back({node,
        distributionStyle == DistributionStyle::Weighted ? scoutProviderWeight(node) : uint8_t{1}});
    if (consensus) {
      for (const auto& assignment : assignments) {
        if (assignment.node == nullptr) {
          localScoutFirstHost = 1;
          localScoutLastHost = 254;
          localScoutAssigned = localHostScan.startRange(1, 254);
        } else {
          RemoteScoutJob job;
          job.nodeId = assignment.node->deviceId;
          job.firstHost = 1;
          job.lastHost = 254;
          remoteScoutJobs.push_back(job);
          requestScoutUpdate(remoteScoutJobs.back(), true);
        }
      }
      scoutExecutor = "consensus";
      scoutRemoteNodeId = "consensus";
      mergeScoutProgress();
      lastScoutPollMs = millis();
      draw();
      return;
    }
    if (assignments.empty()) {
      scoutStatus = "No available scan provider";
      scoutRunning = false;
      draw();
      return;
    }
    uint16_t totalWeight = 0;
    for (const auto& assignment : assignments) totalWeight += assignment.weight;
    uint16_t first = 1;
    uint16_t remainingWeight = totalWeight;
    for (size_t index = 0; index < assignments.size(); ++index) {
      const uint16_t remainingHosts = 255 - first;
      const uint16_t count = index + 1 == assignments.size() ? remainingHosts :
          std::max<uint16_t>(1, remainingHosts * assignments[index].weight / remainingWeight);
      const uint8_t last = static_cast<uint8_t>(first + count - 1);
      if (assignments[index].node == nullptr) {
        localScoutFirstHost = first;
        localScoutLastHost = last;
        localScoutAssigned = localHostScan.startRange(first, count);
      } else {
        RemoteScoutJob job;
        job.nodeId = assignments[index].node->deviceId;
        job.firstHost = static_cast<uint8_t>(first);
        job.lastHost = last;
        remoteScoutJobs.push_back(job);
        requestScoutUpdate(remoteScoutJobs.back(), true);
      }
      first += count;
      remainingWeight -= assignments[index].weight;
    }
    scoutExecutor = "distributed";
    scoutRemoteNodeId = "distributed";
  }
  mergeScoutProgress();
  if (!localScoutAssigned && remoteScoutJobs.empty()) {
    scoutRunning = false;
    scoutStatus = "No available scan provider";
  }
  lastScoutPollMs = millis();
  draw();
}

void startConnecting() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  connectStartedMs = millis();
  screen = ScreenState::Connecting;
  notice = "";
  draw();
}

void workOffline() {
  WiFi.disconnect(true, false);
  wifiRestorePending = false;
  wifiSsid = preferences.getString("ssid", "");
  wifiPassword = preferences.getString("password", "");
  input = "";
  screen = ScreenState::Home;
  selection = 0;
  notice = "Offline field mode";
  draw();
}

void beginProvisioning() {
  WiFi.disconnect(true, false);
  wifiSsid = "";
  wifiPassword = "";
  input = "";
  screen = ScreenState::ProvisionSsid;
  notice = "";
  draw();
}

bool pressedLetter(const Keyboard_Class::KeysState& keys, char wanted) {
  for (const char value : keys.word) {
    if (tolower(static_cast<unsigned char>(value)) == wanted) return true;
  }
  return false;
}

bool validProjectId(const String& value) {
  if (value.isEmpty() || value.length() > 32) return false;
  for (const char character : value) {
    if (!isalnum(static_cast<unsigned char>(character)) && character != '-' &&
        character != '_' && character != '.') return false;
  }
  return true;
}

void loadProjects() {
  projects.clear();
  if (!sdAvailable) return;
  SD.mkdir("/reconclave/projects");
  File root = SD.open("/reconclave/projects");
  if (!root || !root.isDirectory()) return;
  for (File entry = root.openNextFile(); entry; entry = root.openNextFile()) {
    if (!entry.isDirectory()) { entry.close(); continue; }
    String id = entry.name();
    const int slash = id.lastIndexOf('/');
    if (slash >= 0) id = id.substring(slash + 1);
    entry.close();
    File manifest = SD.open("/reconclave/projects/" + id + "/manifest.json");
    if (!manifest) continue;
    JsonDocument document;
    const DeserializationError error = deserializeJson(document, manifest);
    manifest.close();
    if (error || String(document["project_id"] | "") != id) continue;
    ProjectScope project;
    project.projectId = id;
    project.network = String(document["scope"]["allow"][0] | "");
    project.revision = document["scope"]["revision"] | 1;
    if (!project.network.isEmpty()) projects.push_back(project);
  }
  root.close();
  std::sort(projects.begin(), projects.end(), [](const ProjectScope& left,
                                                  const ProjectScope& right) {
    return left.projectId < right.projectId;
  });
  activeProjectId = preferences.getString("active_project", "");
  if (!activeProjectId.isEmpty() && std::none_of(projects.begin(), projects.end(),
      [](const ProjectScope& project) { return project.projectId == activeProjectId; })) {
    activeProjectId = "";
  }
}

bool createProject(const String& projectId) {
  if (!sdAvailable || !validProjectId(projectId)) return false;
  const String directory = "/reconclave/projects/" + projectId;
  if (SD.exists(directory)) return false;
  if (!SD.mkdir(directory)) return false;
  const IPAddress local = WiFi.localIP();
  const String network = String(local[0]) + "." + String(local[1]) + "." +
      String(local[2]) + ".0/24";
  JsonDocument document;
  document["project_id"] = projectId;
  document["created_by"] = deviceId;
  document["created_at_ms"] = static_cast<uint64_t>(millis()) + 1;
  JsonObject scope = document["scope"].to<JsonObject>();
  scope["scope_id"] = "default";
  scope["revision"] = 1;
  scope["allow"].to<JsonArray>().add(network);
  scope["exclude"].to<JsonArray>();
  scope["operations"].to<JsonArray>().add("net.discovery.scan");
  File manifest = SD.open(directory + "/manifest.json", FILE_WRITE);
  if (!manifest) return false;
  const bool written = serializeJson(document, manifest) > 0;
  manifest.close();
  if (!written) return false;
  File audit = SD.open(directory + "/audit.jsonl", FILE_APPEND);
  if (audit) {
    audit.println(String("{\"event\":\"project.created\",\"project_id\":\"") +
                  projectId + "\",\"scope_revision\":1}");
    audit.close();
  }
  loadProjects();
  activeProjectId = projectId;
  preferences.putString("active_project", activeProjectId);
  scoutBaseline.clear();
  scoutBaselineLoaded = false;
  return true;
}

void appendProjectAudit(const char* event, JsonVariantConst detail) {
  if (!sdAvailable || activeProjectId.isEmpty() || !validProjectId(activeProjectId)) return;
  File audit = SD.open("/reconclave/projects/" + activeProjectId + "/audit.jsonl", FILE_APPEND);
  if (!audit) return;
  JsonDocument document;
  document["event"] = event;
  document["project_id"] = activeProjectId;
  document["source_node"] = deviceId;
  document["timestamp_ms"] = static_cast<uint64_t>(millis()) + 1;
  document["detail"].set(detail);
  serializeJson(document, audit);
  audit.println();
  audit.close();
}

void mountEvidence() {
  cap::stop();
  cap::deselect();
  evidenceFiles.clear();
  SD.end();
  SPI.end();
  sdAvailable = false;
  pinMode(kSdCsPin, OUTPUT);
  digitalWrite(kSdCsPin, HIGH);
  // EXT SPI shares the bus on Cardputer ADV; its CS must not float low.
  pinMode(kSdCompatibilityPin, OUTPUT);
  digitalWrite(kSdCompatibilityPin, HIGH);
  delay(20);
  SPI.begin(kSdClockPin, kSdMisoPin, kSdMosiPin, kSdCsPin);
  sdAvailable = SD.begin(kSdCsPin, SPI, kSdFrequency) &&
      SD.cardType() != CARD_NONE;
  if (!sdAvailable) {
    fieldStatus = "microSD mount failed";
    return;
  }
  SD.mkdir("/reconclave");
  SD.mkdir("/reconclave/evidence");
  SD.mkdir("/reconclave/projects");
  File directory = SD.open("/reconclave/evidence");
  if (!directory || !directory.isDirectory()) {
    fieldStatus = "Evidence directory unavailable";
    return;
  }
  for (File file = directory.openNextFile(); file; file = directory.openNextFile()) {
    if (!file.isDirectory()) evidenceFiles.push_back({String(file.name()), file.size()});
    file.close();
  }
  directory.close();
  loadProjects();
  fieldStatus = String(evidenceFiles.size()) + " evidence files";
}

String csvField(String value) {
  value.replace("\"", "\"\"");
  return "\"" + value + "\"";
}

bool openEvidenceCsv(const char* kind, File& file, String& path) {
  if (!sdAvailable) mountEvidence();
  if (!sdAvailable) return false;
  static uint32_t sequence = 0;
  for (unsigned attempt = 0; attempt < 100; ++attempt) {
    path = "/reconclave/evidence/" + String(kind) + "-" +
        String(nodeBootNonceHex).substring(0, 8) + "-" + String(++sequence) + ".csv";
    if (SD.exists(path)) continue;
    file = SD.open(path, FILE_WRITE);
    return static_cast<bool>(file);
  }
  return false;
}

// Presets use bounded plain-text files: one type byte followed by the payload.
// New slots are committed by rename only after a checked write and readback.
String nfcPresetPath(size_t slot) {
  return "/reconclave/nfc-presets/" + String(unsigned(slot + 1)) + ".txt";
}
bool loadNfcPresets() {
  if (!sdAvailable) mountEvidence();
  if (!sdAvailable) return false;
  if (!SD.exists("/reconclave/nfc-presets") && !SD.mkdir("/reconclave/nfc-presets")) return false;
  for (size_t i = 0; i < nfcPresets.size(); ++i) {
    nfcPresets[i] = "";
    nfcPresetOccupied[i] = SD.exists(nfcPresetPath(i));
    File file = SD.open(nfcPresetPath(i), FILE_READ);
    if (!file || file.isDirectory() || file.size() < 2 || file.size() > 121) { file.close(); continue; }
    const size_t length = file.size();
    char bytes[122]{};
    const bool read = file.readBytes(bytes, length) == length;
    file.close();
    bool printable = true;
    for (size_t j = 1; j < length; ++j) if (bytes[j] < 32 || bytes[j] > 126) printable = false;
    const bool url = bytes[0] == 'U';
    if (!read || !printable || (bytes[0] != 'T' && !url) || cap::ndefRecord(bytes + 1, url).empty()) continue;
    nfcPresets[i] = bytes + 1;
    nfcPresetUrls[i] = url;
  }
  return true;
}
void openScreen(ScreenState next);
void saveNfcPreset() {
  auto finish = [&](const String& result) { fieldStatus = result; openScreen(ScreenState::NfcContent); };
  if (cap::ndefRecord(input.c_str(), nfcContentIsUrl()).empty()) { finish("Enter valid text or http(s) URL"); return; }
  if (!loadNfcPresets()) { finish("microSD unavailable"); return; }
  size_t slot = 0;
  while (slot < nfcPresets.size() && nfcPresetOccupied[slot]) ++slot;
  if (slot == nfcPresets.size()) { finish("16 slots full; manage files on SD"); return; }
  const String path = nfcPresetPath(slot), temp = path + ".tmp";
  if (SD.exists(temp) && !SD.remove(temp)) { finish("Cannot clear incomplete save"); return; }
  File file = SD.open(temp, FILE_WRITE);
  if (!file) { finish("Cannot create preset"); return; }
  const String data = String(nfcContentIsUrl() ? "U" : "T") + input;
  CheckedFilePrint output(file);
  bool ok = output.print(data) == data.length() && output.finish();
  file.close();
  File verify = SD.open(temp, FILE_READ);
  char bytes[122]{};
  ok = ok && verify && verify.size() == data.length() &&
      verify.readBytes(bytes, data.length()) == data.length() && data == bytes;
  verify.close();
  ok = ok && !SD.exists(path) && SD.rename(temp, path);
  if (!ok) SD.remove(temp);
  finish(ok ? String("Saved preset ") + String(unsigned(slot + 1)) : "Preset save failed");
}

String scoutBaselinePath() {
  return activeProjectId.isEmpty() ? "/reconclave/evidence/scout-baseline.json" :
      "/reconclave/projects/" + activeProjectId + "/scout-baseline.json";
}

// Best-effort: a Cardputer reboot without SD just resets change detection to
// "no baseline yet" for this boot, same as a brand-new install.
void loadScoutBaseline() {
  if (!sdAvailable) mountEvidence();
  const String path = scoutBaselinePath();
  if (!sdAvailable || !SD.exists(path)) return;
  File file = SD.open(path);
  if (!file) return;
  JsonDocument document;
  const DeserializationError error = deserializeJson(document, file);
  file.close();
  if (error) return;
  for (JsonVariant host : document["hosts"].as<JsonArray>()) {
    scoutBaseline.push_back(String(host.as<const char*>()));
  }
}

void saveScoutBaseline() {
  if (!sdAvailable) mountEvidence();
  if (!sdAvailable) return;
  SD.mkdir("/reconclave/evidence");
  File file = SD.open(scoutBaselinePath(), FILE_WRITE);
  if (!file) return;
  JsonDocument document;
  JsonArray hosts = document["hosts"].to<JsonArray>();
  for (const String& host : scoutBaseline) hosts.add(host);
  serializeJson(document, file);
  file.close();
}

// At-rest AES-256-GCM encryption for evidence written to the (removable) microSD card --
// RC_STORAGE_KEY is provisioned per-device by tools/provision_fleet.py, independent of any
// coordinator pairing so it survives re-pairing. Written in the exact frame format
// tools/desktop-node/encrypted_spool.py's EncryptedSpool reads (one JSON object per line:
// v, node, nonce, ciphertext -- ciphertext already carries its GCM tag appended, matching
// how Python's AESGCM.encrypt concatenates them), so an operator who pulls the card can
// decrypt it with EncryptedSpool(directory, node_id=deviceId,
// key=bytes.fromhex(<the provisioned hex key>)). Per platform-roadmap.md Phase 5.
bool appendEncryptedEvidenceLine(File& file, const String& plaintext) {
  constexpr size_t kNonceBytes = 12;
  constexpr size_t kTagBytes = 16;
  uint8_t nonce[kNonceBytes];
  esp_fill_random(nonce, sizeof(nonce));
  std::vector<uint8_t> combined(plaintext.length() + kTagBytes);
  const String aad = String("reconclave-spool/v1|") + deviceId;
  mbedtls_gcm_context ctx;
  mbedtls_gcm_init(&ctx);
  bool ok = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, RC_STORAGE_KEY, 256) == 0;
  if (ok) {
    ok = mbedtls_gcm_crypt_and_tag(
        &ctx, MBEDTLS_GCM_ENCRYPT, plaintext.length(), nonce, sizeof(nonce),
        reinterpret_cast<const uint8_t*>(aad.c_str()), aad.length(),
        reinterpret_cast<const uint8_t*>(plaintext.c_str()),
        combined.data(), kTagBytes, combined.data() + plaintext.length()) == 0;
  }
  mbedtls_gcm_free(&ctx);
  if (!ok) return false;
  JsonDocument frame;
  frame["v"] = 1;
  frame["node"] = deviceId;
  frame["nonce"] = base64::encode(nonce, sizeof(nonce));
  frame["ciphertext"] = base64::encode(combined.data(), combined.size());
  String line;
  serializeJson(frame, line);
  return file.println(line) == line.length() + 2;
}

// Evidence Collector sink: append-only, never overwrites or deduplicates. Records
// pushed here may originate from any node's job, not just this device's own scans.
bool writeEvidenceRecord(JsonVariantConst evidence, String& errorMessage) {
  if (!sdAvailable) mountEvidence();
  if (!sdAvailable) {
    errorMessage = "microSD unavailable";
    return false;
  }
  SD.mkdir("/reconclave/evidence");
  File file = SD.open("/reconclave/evidence/collected.jsonl", FILE_APPEND);
  if (!file) {
    errorMessage = "evidence log open failed";
    return false;
  }
  String encoded;
  serializeJson(evidence, encoded);
  bool encrypted = appendEncryptedEvidenceLine(file, encoded);
  file.flush();
  encrypted &= file.getWriteError() == 0;
  file.close();
  if (!encrypted) {
    errorMessage = "evidence encryption/write failed";
    return false;
  }
  const String projectId = evidence["project_id"] | "";
  if (validProjectId(projectId) && SD.exists("/reconclave/projects/" + projectId)) {
    File projectEvidence = SD.open("/reconclave/projects/" + projectId +
                                   "/evidence.jsonl", FILE_APPEND);
    if (projectEvidence) {
      appendEncryptedEvidenceLine(projectEvidence, encoded);
      projectEvidence.close();
    }
  }
  return true;
}

void saveWifiEvidence() {
  if (wifiObservations.empty()) {
    fieldStatus = "Nothing to save; scan first";
    draw();
    return;
  }
  File file;
  String path;
  if (!openEvidenceCsv("wifi", file, path)) {
    fieldStatus = "microSD export failed";
    draw();
    return;
  }
  CheckedFilePrint output(file);
  output.println("ssid,bssid,channel,rssi_dbm,security,observer_node");
  for (const auto& ap : wifiObservations) {
    output.printf("%s,%s,%ld,%ld,%s,%s\n", csvField(ap.ssid).c_str(),
                ap.bssid.c_str(), static_cast<long>(ap.channel),
                static_cast<long>(ap.rssi), authName(ap.auth), deviceId.c_str());
  }
  const bool ok = output.finish();
  file.close();
  fieldStatus = ok ? "Saved " + path.substring(path.lastIndexOf('/') + 1) : "microSD write failed";
  draw();
}

void saveCapEvidence(bool nfc) {
  bool haveSamples = false;
  for (unsigned i = 0; i < 4; ++i) haveSamples |= cap::band(i).samples != 0;
  if (nfc ? cap::tags().empty() : !haveSamples) {
    fieldStatus = "Nothing to save; scan first";
    draw();
    return;
  }
  File file;
  String path;
  if (!openEvidenceCsv(nfc ? "nfc" : "subghz", file, path)) {
    fieldStatus = "microSD export failed";
    draw();
    return;
  }
  CheckedFilePrint output(file);
  bool ok;
  if (nfc) {
    ok = output.println("identifier,type_hint,protocol,detail,observer_node") > 0;
    for (const auto& tag : cap::tags())
      ok &= output.printf("%s,%s,%s,%s,%s\n", csvField(tag.uid).c_str(),
          csvField(tag.type).c_str(), tag.protocol.c_str(), csvField(tag.detail).c_str(), deviceId.c_str()) > 0;
  } else {
    ok = output.println("frequency_mhz,last_rssi_dbm,peak_rssi_dbm,samples,observer_node,threshold_dbm,window_samples,above_threshold_percent") > 0;
    for (unsigned i = 0; i < 4; ++i) {
      const auto& band = cap::band(i);
      if (band.samples) ok &= output.printf("%.2f,%.1f,%.1f,%lu,%s,%d,%u,%u\n", band.mhz,
          band.rssi, band.peak, static_cast<unsigned long>(band.samples), deviceId.c_str(),
          band.threshold, unsigned(band.history.size()), band.history.activityPercent(band.threshold)) > 0;
    }
  }
  ok &= output.finish();
  file.close();
  fieldStatus = ok ? "Saved " + path.substring(path.lastIndexOf('/') + 1) : "microSD write failed";
  draw();
}

void saveRadioTrace() {
  const auto& band = cap::band(cap::selectedBand());
  if (!band.history.size()) { fieldStatus = "No RF samples to export"; draw(); return; }
  File file;
  String path;
  if (!openEvidenceCsv("subghz-trace", file, path)) {
    fieldStatus = "microSD export failed";
    draw();
    return;
  }
  CheckedFilePrint output(file);
  bool ok = output.println("frequency_mhz,uptime_ms,rssi_dbm,observer_node,threshold_dbm,above_threshold") > 0;
  for (size_t i = 0; i < band.history.size(); ++i)
    ok &= output.printf("%.2f,%lu,%.1f,%s,%d,%u\n", band.mhz,
        static_cast<unsigned long>(band.history.sampledAt(i)), band.history.at(i), deviceId.c_str(),
        band.threshold, unsigned(band.history.at(i) >= band.threshold)) > 0;
  ok &= output.finish();
  file.close();
  fieldStatus = ok ? "Saved RF trace" : "microSD write failed";
  draw();
}

void saveBleEvidence() {
  if (bleObservations.empty()) {
    fieldStatus = "Nothing to save; scan first";
    draw();
    return;
  }
  File file;
  String path;
  if (!openEvidenceCsv("ble", file, path)) {
    fieldStatus = "microSD export failed";
    draw();
    return;
  }
  CheckedFilePrint output(file);
  output.println("name,address,address_type,advertisement_type,rssi_dbm,connectable,service_count,services,payload_bytes,manufacturer,observer_node");
  for (const auto& device : bleObservations) {
    output.printf("%s,%s,%u,%u,%d,%s,%u,%s,%u,%s,%s\n", csvField(device.name).c_str(),
                device.address.c_str(), device.addressType, device.advertisementType, device.rssi,
                device.connectable ? "true" : "false",
                device.serviceCount, csvField(device.services).c_str(),
                static_cast<unsigned>(device.payloadLength),
                csvField(device.manufacturer).c_str(), deviceId.c_str());
  }
  const bool ok = output.finish();
  file.close();
  fieldStatus = ok ? "Saved " + path.substring(path.lastIndexOf('/') + 1) : "microSD write failed";
  draw();
}

void saveScoutEvidence() {
  if (discoveredHosts.empty()) {
    scoutStatus = "No host results to save";
    draw();
    return;
  }
  File file;
  String path;
  if (!openEvidenceCsv("p4-hosts", file, path)) {
    scoutStatus = "microSD export failed";
    draw();
    return;
  }
  CheckedFilePrint output(file);
  output.println("ip,responsive,observer_node,vantage");
  for (const String& host : discoveredHosts) {
    bool attributed = false;
    if (std::find(localScoutHosts.begin(), localScoutHosts.end(), host) != localScoutHosts.end()) {
      output.printf("%s,true,%s,local\n", host.c_str(), deviceId.c_str());
      attributed = true;
    }
    for (const auto& job : remoteScoutJobs) {
      if (std::find(job.hosts.begin(), job.hosts.end(), host) != job.hosts.end()) {
        output.printf("%s,true,%s,remote\n", host.c_str(), job.nodeId.c_str());
        attributed = true;
      }
    }
    // Retained results from an earlier recurring pass may no longer have a
    // provider in the current pass. Do not guess and misattribute them.
    if (!attributed) output.printf("%s,true,,unknown\n", host.c_str());
  }
  const bool ok = output.finish();
  file.close();
  scoutStatus = ok ? "Saved " + path.substring(path.lastIndexOf('/') + 1) : "microSD write failed";
  draw();
}

bool observationScreen() {
  return screen == ScreenState::WifiResults || screen == ScreenState::WifiChannels ||
      screen == ScreenState::BleResults;
}

void finishWifiScan(int count) {
  wifiScanning = false;
  if (count >= 0) {
    wifiObservations.clear();
    // Sort scan indices first so the bounded result list keeps strongest APs.
    std::vector<int> indices;
    for (int i = 0; i < count; ++i) indices.push_back(i);
    std::sort(indices.begin(), indices.end(), [](int a, int b) { return WiFi.RSSI(a) > WiFi.RSSI(b); });
    for (size_t i = 0; i < indices.size() && i < 128; ++i) {
      const int index = indices[i];
      WifiObservation observation;
      observation.ssid = WiFi.SSID(index).isEmpty() ? "<hidden>" : WiFi.SSID(index);
      observation.bssid = WiFi.BSSIDstr(index);
      observation.rssi = WiFi.RSSI(index);
      observation.channel = WiFi.channel(index);
      observation.auth = WiFi.encryptionType(index);
      wifiObservations.push_back(observation);
    }
    fieldStatus = String(wifiObservations.size()) + (count > 128 ? " strongest APs (128 limit)" : " access points");
    if (screen == ScreenState::WifiResults) selection = 0;
  } else fieldStatus = "Wi-Fi scan stopped/failed";
  WiFi.scanDelete();
  if (wifiScanWasOff) WiFi.mode(WIFI_OFF);
  if (observationScreen()) draw();
}

void scanWifi() {
  if (wifiScanning) {
    esp_wifi_scan_stop();
    finishWifiScan(-1);
    return;
  }
  if (bleScanning) { fieldStatus = "Wait for BLE scan to finish"; draw(); return; }
  if (screen == ScreenState::WifiDetail) screen = ScreenState::WifiResults;
  wifiScanWasOff = WiFi.getMode() == WIFI_OFF;
  if (wifiScanWasOff) WiFi.mode(WIFI_STA);
  WiFi.scanDelete();
  // Passive async scan leaves keyboard, fleet requests and display responsive.
  const int result = WiFi.scanNetworks(true, true, true);
  wifiScanStartedMs = millis();
  wifiScanning = result == WIFI_SCAN_RUNNING;
  fieldStatus = "Scanning Wi-Fi... Q to cancel";
  if (!wifiScanning) finishWifiScan(result);
  draw();
}

void restoreWifiAfterBle() {
  if (!restoreAfterBle || wifiSsid.isEmpty()) {
    WiFi.mode(WIFI_OFF);
    wifiRestorePending = false;
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  wifiRestorePending = true;
}

void finishBleScan(bool cancelled) {
  NimBLEScan* scanner = NimBLEDevice::getScan();
  if (cancelled && !scanner->stop()) {
    fieldStatus = "BLE stop failed; waiting for scan";
    draw();
    return;
  }
  // Called only after scanning ends; callbacks no longer mutate the results.
  NimBLEScanResults results = scanner->getResults();
  bleObservations.clear();
  for (int index = 0; index < results.getCount(); ++index) {
    const NimBLEAdvertisedDevice* advertised = results.getDevice(index);
    if (advertised == nullptr) continue;
    BleObservation observation;
    observation.address = advertised->getAddress().toString().c_str();
    observation.name = advertised->haveName() ? advertised->getName().c_str() : "<unnamed>";
    observation.rssi = advertised->getRSSI();
    const uint8_t type = advertised->getAdvType();
    observation.addressType = advertised->getAddressType();
    observation.advertisementType = type;
    observation.connectable = type == 0 || type == 1;
    observation.payloadLength = advertised->getPayload().size();
    observation.serviceCount = advertised->getServiceUUIDCount();
    for (uint8_t service = 0; service < observation.serviceCount && service < 2; ++service) {
      if (!observation.services.isEmpty()) observation.services += " ";
      observation.services += advertised->getServiceUUID(service).toString().c_str();
    }
    if (advertised->haveManufacturerData()) {
      const std::string data = advertised->getManufacturerData();
      if (data.size() >= 2) {
        const uint16_t company = static_cast<uint8_t>(data[0]) |
            (static_cast<uint16_t>(static_cast<uint8_t>(data[1])) << 8);
        char label[16];
        snprintf(label, sizeof(label), "company %04X", company);
        observation.manufacturer = label;
      }
    }
    bleObservations.push_back(observation);
  }
  std::sort(bleObservations.begin(), bleObservations.end(),
            [](const BleObservation& left, const BleObservation& right) {
              return left.rssi > right.rssi;
            });
  scanner->clearResults();
  NimBLEDevice::deinit(true);
  bleScanning = false;
  fieldStatus = String(bleObservations.size()) +
      (cancelled ? " BLE devices (cancelled)" : " BLE devices (max 64)");
  restoreWifiAfterBle();
  if (screen == ScreenState::BleResults) selection = 0;
  if (observationScreen()) draw();
}

void scanBle() {
  if (bleScanning) { finishBleScan(true); return; }
  if (wifiScanning || localHostScan.active() || remoteHostScan.active() || portScan.active()) {
    fieldStatus = "Finish network scans before BLE";
    draw();
    return;
  }
  if (screen == ScreenState::BleDetail) screen = ScreenState::BleResults;
  restoreAfterBle = WiFi.getMode() != WIFI_OFF && !wifiSsid.isEmpty();
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_OFF);
  NimBLEDevice::init("");
  NimBLEScan* scanner = NimBLEDevice::getScan();
  if (scanner == nullptr) {
    fieldStatus = "BLE scanner unavailable";
    NimBLEDevice::deinit(true);
    restoreWifiAfterBle();
    draw();
    return;
  }
  scanner->clearResults();
  scanner->setMaxResults(64);
  scanner->setActiveScan(false);
  scanner->setInterval(100);
  scanner->setWindow(80);
  bleScanning = scanner->start(5000, false);
  if (!bleScanning) {
    NimBLEDevice::deinit(true);
    restoreWifiAfterBle();
  }
  fieldStatus = bleScanning ? "Scanning BLE... Q to cancel" : "BLE scan failed to start";
  draw();
}

void updateObservationScans() {
  if (wifiScanning) {
    const int count = WiFi.scanComplete();
    if (count != WIFI_SCAN_RUNNING) finishWifiScan(count);
    else if (millis() - wifiScanStartedMs > 15000) {
      esp_wifi_scan_stop();
      finishWifiScan(-1);
    }
  }
  if (bleScanning && !NimBLEDevice::getScan()->isScanning()) finishBleScan(false);
}

void loadEvidencePreview() {
  previewLines.clear();
  previewLine = 0;
  previewTruncated = false;
  File file = SD.open(selectedEvidencePath());
  if (!file || file.isDirectory()) return;
  constexpr size_t kMaxLines = 64;
  constexpr size_t kMaxLineLength = 160;
  String line;
  while (file.available() && previewLines.size() < kMaxLines) {
    const char value = static_cast<char>(file.read());
    if (value == '\n') {
      if (line.endsWith("\r")) line.remove(line.length() - 1);
      previewLines.push_back(line);
      line = "";
    } else if (line.length() < kMaxLineLength) {
      line += value;
    }
  }
  if (!line.isEmpty() && previewLines.size() < kMaxLines) previewLines.push_back(line);
  previewTruncated = file.available();
  file.close();
}

bool backPressed(const Keyboard_Class::KeysState& keys) {
  return keys.esc || keys.backspace || pressedLetter(keys, 'q');
}

bool leftPressed(const Keyboard_Class::KeysState& keys) {
  return keys.left || pressedLetter(keys, ',');
}

bool rightPressed(const Keyboard_Class::KeysState& keys) {
  return keys.right || pressedLetter(keys, '/');
}

void moveSelection(const Keyboard_Class::KeysState& keys, size_t count) {
  if (count == 0) return;
  const size_t previous = selection;
  if (keys.up || pressedLetter(keys, 'w') || pressedLetter(keys, 'k') ||
      pressedLetter(keys, ';')) {
    selection = selection == 0 ? count - 1 : selection - 1;
  } else if (keys.down || pressedLetter(keys, 's') || pressedLetter(keys, 'j') ||
             pressedLetter(keys, '.')) {
    selection = (selection + 1) % count;
  }
  if (selection != previous) playUiCue(UiCue::Move);
}

void openScreen(ScreenState next) {
  playUiCue(UiCue::Open);
  selection = 0;
  screen = next;
  draw();
}

void returnToScreen(ScreenState next) {
  playUiCue(UiCue::Back);
  screen = next;
  draw();
}

void returnToMenu(ScreenState next, size_t menuSelection) {
  playUiCue(UiCue::Back);
  selection = menuSelection;
  screen = next;
  draw();
}

void openContextMenu() {
  playUiCue(UiCue::Open);
  contextOrigin = screen;
  contextOriginSelection = selection;
  selection = 0;
  screen = ScreenState::ContextMenu;
  draw();
}

void closeContextMenu() {
  playUiCue(UiCue::Back);
  selection = contextOriginSelection;
  screen = contextOrigin;
  draw();
}

void cycleScoutTarget(bool forward) {
  std::vector<String> available{"auto", "local"};
  for (const auto& node : remoteNodes) {
    if (nodeHasCapability(node, "net.discovery.scan") &&
        capabilityEnabled(node.deviceId, "net.discovery.scan"))
      available.push_back(node.deviceId);
  }
  size_t current = 0;
  for (size_t index = 0; index < available.size(); ++index) {
    if (available[index] == scoutTargetId) current = index;
  }
  current = forward ? (current + 1) % available.size() :
      (current + available.size() - 1) % available.size();
  scoutTargetId = available[current];
}

void cyclePortProfile(bool forward) {
  if (forward)
    portProfile = portProfile == PortProfile::Web ? PortProfile::Common :
        (portProfile == PortProfile::Common ? PortProfile::Extended : PortProfile::Web);
  else
    portProfile = portProfile == PortProfile::Web ? PortProfile::Extended :
        (portProfile == PortProfile::Extended ? PortProfile::Common : PortProfile::Web);
}

bool startSelectedPortScan() {
  IPAddress address;
  const uint16_t* ports = nullptr;
  size_t count = 0;
  selectedPortList(ports, count);
  openPorts.clear();
  portStatus = "Starting...";
  portRunning = address.fromString(selectedHost) && portScan.start(address, ports, count);
  if (!portRunning) portStatus = "Unable to start";
  return portRunning;
}

void handleContextInput(const Keyboard_Class::KeysState& keys) {
  const size_t count = contextItemCount();
  moveSelection(keys, count);
  if (keys.tab || backPressed(keys)) {
    closeContextMenu();
    return;
  }
  const bool adjust = leftPressed(keys) || rightPressed(keys) || keys.enter;
  if (adjust && contextOrigin == ScreenState::Scout && selection == 0) {
    cycleProjectScope(!leftPressed(keys));
  } else if (adjust && contextOrigin == ScreenState::Scout && selection == 1) {
    cycleScoutExecution(!leftPressed(keys));
  } else if (adjust && contextOrigin == ScreenState::Scout && selection == 2) {
    cycleScoutTarget(!leftPressed(keys));
  } else if (adjust && contextOrigin == ScreenState::Scout && selection == 3) {
    distributionStyle = distributionStyle == DistributionStyle::Equal ?
        DistributionStyle::Weighted : DistributionStyle::Equal;
  } else if (adjust && contextOrigin == ScreenState::Scout && selection == 4) {
    cycleScoutInterval(!leftPressed(keys));
    if (scoutIntervalMinutes == 0) {
      // Explicitly stopping recurring: tell every remote provider to stop its
      // repeating job rather than leaving it running unattended.
      for (const auto& job : remoteScoutJobs) requestScoutCancel(job);
    }
  } else if (adjust && contextOrigin == ScreenState::Scout && selection == 5) {
    scoutRecurringPolicy = scoutRecurringPolicy == RecurringPolicy::Independent ?
        RecurringPolicy::Callback : RecurringPolicy::Independent;
  } else if (adjust && ((contextOrigin == ScreenState::Scout && selection == 6) ||
                        contextOrigin == ScreenState::HostDetail ||
                        (contextOrigin == ScreenState::PortResults && selection == 0))) {
    cyclePortProfile(!leftPressed(keys));
  } else if (keys.enter) {
    const ScreenState origin = contextOrigin;
    const size_t action = selection;
    closeContextMenu();
    if (origin == ScreenState::Scout && action == 7) saveScoutEvidence();
    else if (origin == ScreenState::NfcResults || origin == ScreenState::SubGhz) {
      if (action == 5 && origin == ScreenState::NfcResults) {
        cap::readNdefContent(nfcReadText, nfcReadUrl, nfcReadUid);
        nfcReadRow = 0;
        openScreen(ScreenState::NfcViewer);
      } else if (action == 4 && origin == ScreenState::NfcResults) {
        cap::stop();
        openScreen(ScreenState::NfcTools);
      } else if (action == 1) saveCapEvidence(origin == ScreenState::NfcResults);
      else if (action == 2) {
        if (origin == ScreenState::NfcResults) cap::cycleNfcFilter(true);
        else saveRadioTrace();
      } else if (action == 3) {
        if (origin == ScreenState::NfcResults) { cap::clearTags(); selection = 0; }
        else cap::clearRadioHistory();
        fieldStatus = "History cleared";
      } else if (origin == ScreenState::NfcResults) {
        if (cap::scanningNfc()) cap::stop(); else cap::scanNfc();
      }
      else if (cap::receiving()) cap::stop();
      else cap::startRadio(cap::selectedBand());
      draw();
    }
    else if (origin == ScreenState::PortResults) startSelectedPortScan();
    else if (origin == ScreenState::WifiResults || origin == ScreenState::WifiDetail) {
      if (action == 0) scanWifi(); else saveWifiEvidence();
    } else if (origin == ScreenState::WifiChannels) {
      scanWifi();
    } else if (origin == ScreenState::BleResults || origin == ScreenState::BleDetail) {
      if (action == 0) scanBle(); else saveBleEvidence();
    } else if (origin == ScreenState::Reconclave) {
      if (action == 0) discover(); else beginGrovePairing();
    } else if (origin == ScreenState::NodeDetail) {
      RemoteNode* node = selectedRemoteNode();
      if (node && nodeHasCapability(*node, "system.info")) requestSystemInfo(*node);
      else {
        notice = selectedNodeId == deviceId ? "Local status refreshed" :
                                                "Management unavailable";
        draw();
      }
    } else if (origin == ScreenState::Evidence) {
      if (sdAvailable) SD.end();
      sdAvailable = false;
      mountEvidence();
    } else if (origin == ScreenState::Projects) {
      if (action == 0) {
        input = "";
        openScreen(ScreenState::ProvisionProject);
      } else {
        loadProjects();
        draw();
      }
    }
    return;
  }
  draw();
}

void goBack() {
  if (screen == ScreenState::NfcPresets) { openScreen(ScreenState::NfcContent); return; }
  if (screen == ScreenState::NfcViewer) {
    cap::stop(); returnToMenu(ScreenState::NfcResults, 0); return;
  }
  if (screen == ScreenState::NfcTools) {
    cap::stop();
    returnToMenu(ScreenState::NfcResults, 0);
    return;
  }
  if (screen == ScreenState::NfcWriteConfirm) {
    cap::stop();
    fieldStatus = "Write cancelled; edit or retry";
    openScreen(ScreenState::NfcContent);
    return;
  }
  if (screen == ScreenState::NfcContent || screen == ScreenState::NfcEmulation) {
    if (screen == ScreenState::NfcContent) nfcPayload = input;
    cap::stop();
    input = "";
    returnToMenu(ScreenState::NfcTools, nfcAction);
    return;
  }
  if ((screen == ScreenState::WifiResults || screen == ScreenState::WifiChannels) && wifiScanning) {
    esp_wifi_scan_stop();
    finishWifiScan(-1);
  }
  if (screen == ScreenState::BleResults && bleScanning) finishBleScan(true);
  if (screen == ScreenState::WifiDetail) returnToScreen(ScreenState::WifiResults);
  else if (screen == ScreenState::BleDetail) returnToScreen(ScreenState::BleResults);
  else if (screen == ScreenState::WifiResults) returnToMenu(ScreenState::Observe, 0);
  else if (screen == ScreenState::WifiChannels) returnToMenu(ScreenState::Observe, 1);
  else if (screen == ScreenState::BleResults) returnToMenu(ScreenState::Observe, 2);
  else if (screen == ScreenState::NfcResults || screen == ScreenState::SubGhz) {
    const bool nfc = screen == ScreenState::NfcResults;
    cap::stop();
    returnToMenu(ScreenState::Observe, nfc ? 3 : 4);
  }
  else if (screen == ScreenState::EvidencePreview) returnToScreen(ScreenState::EvidenceDetail);
  else if (screen == ScreenState::EvidenceDetail) returnToScreen(ScreenState::Evidence);
  else if (screen == ScreenState::ProjectDetail) returnToScreen(ScreenState::Projects);
  else if (screen == ScreenState::PortResults) {
    if (portRunning) portScan.stop();
    portRunning = false;
    returnToScreen(ScreenState::HostDetail);
  } else if (screen == ScreenState::HostDetail) {
    selection = scoutSelection;
    returnToScreen(ScreenState::Scout);
  }
  else if (screen == ScreenState::NodeCapabilities) returnToScreen(ScreenState::NodeDetail);
  else if (screen == ScreenState::NodeDetail) {
    selection = nodeSelection;
    returnToScreen(ScreenState::Reconclave);
  }
  else if (screen == ScreenState::Reconclave) returnToMenu(ScreenState::Home, 0);
  else if (screen == ScreenState::Observe) returnToMenu(ScreenState::Home, 1);
  else if (screen == ScreenState::Scout) returnToMenu(ScreenState::Home, 2);
  else if (screen == ScreenState::Evidence) returnToMenu(ScreenState::Home, 3);
  else if (screen == ScreenState::Projects) returnToMenu(ScreenState::Home, 4);
  else if (screen == ScreenState::FieldKit) returnToMenu(ScreenState::Home, 5);
  else if (screen == ScreenState::NetworkDashboard) returnToMenu(ScreenState::FieldKit, 0);
  else if (screen == ScreenState::System) {
    if (systemOpenedFromSettings) returnToMenu(ScreenState::SettingsDevice, 0);
    else returnToMenu(ScreenState::FieldKit, 1);
  }
  else if (screen == ScreenState::SettingsConnectivity) returnToMenu(ScreenState::Settings, 0);
  else if (screen == ScreenState::SettingsDisplay) returnToMenu(ScreenState::Settings, 1);
  else if (screen == ScreenState::SettingsBoot) returnToMenu(ScreenState::SettingsDisplay, 5);
  else if (screen == ScreenState::SettingsStorage) returnToMenu(ScreenState::Settings, 2);
  else if (screen == ScreenState::SettingsDevice) returnToMenu(ScreenState::Settings, 3);
  else if (screen == ScreenState::SettingsTrust) returnToMenu(ScreenState::Settings, 4);
  else if (screen == ScreenState::ConfirmForgetTrust) returnToMenu(ScreenState::SettingsTrust, 0);
  else if (screen == ScreenState::ProvisionEvidenceKey)
    returnToMenu(ScreenState::SettingsTrust, provisioningExecutionKey ? 1 : 2);
  else if (screen == ScreenState::Settings) returnToMenu(ScreenState::Home, 6);
  else openScreen(ScreenState::Home);
}

void handleApplicationInput(const Keyboard_Class::KeysState& keys) {
  if (screen == ScreenState::ContextMenu) {
    handleContextInput(keys);
    return;
  }
  if (screen == ScreenState::NfcViewer) {
    if (backPressed(keys)) goBack();
    else if (keys.enter) {
      cap::readNdefContent(nfcReadText, nfcReadUrl, nfcReadUid);
      nfcReadRow = 0;
    } else if ((keys.up || pressedLetter(keys, ';')) && nfcReadRow) --nfcReadRow;
    else if ((keys.down || pressedLetter(keys, '.')) && (nfcReadRow + 5) * 37 < nfcReadText.length()) ++nfcReadRow;
    draw(); return;
  }
  if (screen == ScreenState::NfcPresets) {
    moveSelection(keys, 17);
    if (backPressed(keys)) goBack();
    else if (keys.enter) {
      if (!selection) saveNfcPreset();
      else if (!nfcPresets[selection - 1].isEmpty()) {
        input = nfcPresets[selection - 1];
        nfcAction = (nfcAction / 2) * 2 + unsigned(nfcPresetUrls[selection - 1]);
        fieldStatus = "Preset loaded; review then Enter";
        openScreen(ScreenState::NfcContent);
      } else {
        fieldStatus = nfcPresetOccupied[selection - 1] ? "Invalid preset; check file on SD" : "Empty slot; use Save current content";
        openScreen(ScreenState::NfcContent);
      }
    }
    draw(); return;
  }
  if (screen == ScreenState::NfcTools) {
    moveSelection(keys, 6);
    if (backPressed(keys)) goBack();
    else if (keys.enter) {
      nfcAction = selection;
      input = nfcPayload;
      fieldStatus = "";
      openScreen(ScreenState::NfcContent);
    } else draw();
    return;
  }
  if (screen == ScreenState::NfcWriteConfirm) {
    if (backPressed(keys)) goBack();
    else if (keys.enter) {
      fieldStatus = "Writing; keep tag still...";
      header("NFC / WRITING");
      uiCanvas.setCursor(8, 48);
      uiCanvas.print(fieldStatus);
      uiCanvas.pushSprite(0, 0);
      cap::writeNdef(nfcPayload, nfcContentIsUrl());
      fieldStatus = "";
      openScreen(ScreenState::NfcResults);
    }
    return;
  }
  if (screen == ScreenState::NfcEmulation) {
    if (backPressed(keys) || keys.enter) goBack();
    return;
  }
  if (screen == ScreenState::Connecting) {
    if (backPressed(keys)) workOffline();
    else if (pressedLetter(keys, 'w')) beginProvisioning();
    return;
  }
  if (keys.tab) {
    openContextMenu();
    return;
  }
  if (screen == ScreenState::Home) {
    moveSelection(keys, 7);
    if (navigationStyle == NavigationStyle::Cards && leftPressed(keys)) {
      selection = selection == 0 ? 6 : selection - 1;
      playUiCue(UiCue::Move);
    } else if (navigationStyle == NavigationStyle::Cards && rightPressed(keys)) {
      selection = (selection + 1) % 7;
      playUiCue(UiCue::Move);
    }
    if (keys.enter) {
      if (selection == 0) {
        openScreen(ScreenState::Reconclave);
        discover();
      }
      else if (selection == 1) openScreen(ScreenState::Observe);
      else if (selection == 2) openScreen(ScreenState::Scout);
      else if (selection == 3) {
        mountEvidence();
        openScreen(ScreenState::Evidence);
      } else if (selection == 4) {
        mountEvidence();
        openScreen(ScreenState::Projects);
      } else if (selection == 5) openScreen(ScreenState::FieldKit);
      else openScreen(ScreenState::Settings);
      return;
    }
  } else if (screen == ScreenState::Reconclave) {
    moveSelection(keys, remoteNodes.size() + 1);
    if (backPressed(keys)) goBack();
    else if (keys.enter) {
      nodeSelection = selection;
      selectedNodeId = selection == 0 ? deviceId : remoteNodes[selection - 1].deviceId;
      openScreen(ScreenState::NodeDetail);
    } else draw();
    return;
  } else if (screen == ScreenState::NodeDetail) {
    if (backPressed(keys)) goBack();
    else if (keys.enter) openScreen(ScreenState::NodeCapabilities);
    return;
  } else if (screen == ScreenState::NodeCapabilities) {
    RemoteNode* node = selectedRemoteNode();
    static const char* const localCapabilities[] = {
        "system.info", "coordination.nodes", "coordination.jobs", "input.keyboard",
        "radio.wifi.scan", "radio.ble.scan", "storage.file.read", "storage.evidence.write",
        "net.discovery.scan"};
    const bool local = selectedNodeId == deviceId;
    const size_t count = local ? sizeof(localCapabilities) / sizeof(localCapabilities[0]) :
        (node ? node->capabilities.size() : 0);
    moveSelection(keys, count);
    if (backPressed(keys)) goBack();
    else if (keys.enter && count > 0) {
      const String capability = local ? String(localCapabilities[selection]) : node->capabilities[selection];
      toggleCapability(selectedNodeId, capability);
      notice = capabilityEnabled(selectedNodeId, capability) ? "Capability enabled" : "Capability disabled";
      draw();
    }
    else draw();
    return;
  } else if (screen == ScreenState::Scout) {
    moveSelection(keys, discoveredHosts.size());
    if (backPressed(keys)) goBack();
    else if (pressedLetter(keys, 'r') && !scoutRunning) {
      startScoutJob();
      return;
    } else if (keys.enter && !scoutRunning && !discoveredHosts.empty()) {
      scoutSelection = selection;
      selectedHost = discoveredHosts[selection];
      openScreen(ScreenState::HostDetail);
      return;
    }
  } else if (screen == ScreenState::HostDetail) {
    if (backPressed(keys)) goBack();
    else if (keys.enter) {
      openPorts.clear();
      portStatus = "Starting...";
      startSelectedPortScan();
      openScreen(ScreenState::PortResults);
      return;
    }
  } else if (screen == ScreenState::PortResults) {
    moveSelection(keys, openPorts.size());
    if (backPressed(keys)) goBack();
  } else if (screen == ScreenState::Observe) {
    moveSelection(keys, 5);
    if (backPressed(keys)) goBack();
    else if (keys.enter && selection == 0) {
      openScreen(ScreenState::WifiResults);
      scanWifi();
      return;
    } else if (keys.enter && selection == 1) {
      openScreen(ScreenState::WifiChannels);
      return;
    } else if (keys.enter && selection == 2) {
      openScreen(ScreenState::BleResults);
      scanBle();
      return;
    } else if (keys.enter && selection == 3) {
      fieldStatus = "";
      cap::scanNfc();
      openScreen(ScreenState::NfcResults);
      return;
    } else if (keys.enter && selection == 4) {
      fieldStatus = "";
      cap::startRadio(cap::selectedBand());
      openScreen(ScreenState::SubGhz);
      return;
    }
  } else if (screen == ScreenState::NfcResults) {
    moveSelection(keys, cap::tags().size());
    if (backPressed(keys)) goBack();
    else if (leftPressed(keys) || rightPressed(keys)) cap::cycleNfcFilter(!leftPressed(keys));
    else if (keys.enter) {
      if (cap::scanningNfc()) cap::stop(); else cap::scanNfc();
    }
  } else if (screen == ScreenState::SubGhz) {
    if (backPressed(keys)) goBack();
    else if (keys.up || pressedLetter(keys, ';') || pressedLetter(keys, 'w')) {
      cap::adjustRadioThreshold(true);
      fieldStatus = "";
    } else if (keys.down || pressedLetter(keys, '.') || pressedLetter(keys, 's')) {
      cap::adjustRadioThreshold(false);
      fieldStatus = "";
    }
    else if (leftPressed(keys) || rightPressed(keys))
      cap::startRadio((cap::selectedBand() + (leftPressed(keys) ? 3 : 1)) % 4);
    else if (keys.enter) {
      if (cap::receiving()) cap::stop();
      else cap::startRadio(cap::selectedBand());
    }
  } else if (screen == ScreenState::WifiResults) {
    moveSelection(keys, wifiObservations.size());
    if (backPressed(keys)) goBack();
    else if (keys.enter && !wifiScanning && !wifiObservations.empty()) {
      screen = ScreenState::WifiDetail;
      draw();
      return;
    }
  } else if (screen == ScreenState::WifiChannels) {
    if (backPressed(keys)) goBack();
  } else if (screen == ScreenState::WifiDetail) {
    if (backPressed(keys)) goBack();
  } else if (screen == ScreenState::BleResults) {
    moveSelection(keys, bleObservations.size());
    if (backPressed(keys)) goBack();
    else if (keys.enter && !bleScanning && !bleObservations.empty()) {
      screen = ScreenState::BleDetail;
      draw();
      return;
    }
  } else if (screen == ScreenState::BleDetail) {
    if (backPressed(keys)) goBack();
  } else if (screen == ScreenState::Evidence) {
    moveSelection(keys, evidenceFiles.size());
    if (backPressed(keys)) goBack();
    else if (keys.enter && !evidenceFiles.empty()) {
      screen = ScreenState::EvidenceDetail;
      draw();
      return;
    }
  } else if (screen == ScreenState::EvidenceDetail) {
    if (backPressed(keys)) {
      goBack();
      return;
    } else if (keys.enter && evidencePreviewable(evidenceFiles[selection].name)) {
      loadEvidencePreview();
      screen = ScreenState::EvidencePreview;
      draw();
      return;
    }
  } else if (screen == ScreenState::EvidencePreview) {
    if (backPressed(keys)) {
      goBack();
      return;
    }
    if (!previewLines.empty()) {
      if (keys.up || pressedLetter(keys, 'w') || pressedLetter(keys, 'k') ||
          pressedLetter(keys, ';')) {
        if (previewLine > 0) --previewLine;
      } else if (keys.down || pressedLetter(keys, 's') || pressedLetter(keys, 'j') ||
                 pressedLetter(keys, '.')) {
        if (previewLine + 1 < previewLines.size()) ++previewLine;
      }
    }
  } else if (screen == ScreenState::Projects) {
    moveSelection(keys, projects.size());
    if (backPressed(keys)) goBack();
    else if (keys.enter && !projects.empty()) {
      projectSelection = selection;
      openScreen(ScreenState::ProjectDetail);
    }
  } else if (screen == ScreenState::ProjectDetail) {
    if (backPressed(keys)) goBack();
    else if (keys.enter && projectSelection < projects.size()) {
      activeProjectId = projects[projectSelection].projectId;
      preferences.putString("active_project", activeProjectId);
      scoutBaseline.clear();
      scoutBaselineLoaded = false;
      playUiCue(UiCue::Confirm);
      draw();
    }
  } else if (screen == ScreenState::FieldKit) {
    moveSelection(keys, 2);
    if (backPressed(keys)) goBack();
    else if (keys.enter && selection == 0) openScreen(ScreenState::NetworkDashboard);
    else if (keys.enter && selection == 1) {
      systemOpenedFromSettings = false;
      openScreen(ScreenState::System);
    }
  } else if (screen == ScreenState::NetworkDashboard) {
    if (backPressed(keys)) goBack();
  } else if (screen == ScreenState::System) {
    if (backPressed(keys)) goBack();
  } else if (screen == ScreenState::Settings) {
    moveSelection(keys, 5);
    if (backPressed(keys)) goBack();
    else if (keys.enter && selection == 0) openScreen(ScreenState::SettingsConnectivity);
    else if (keys.enter && selection == 1) openScreen(ScreenState::SettingsDisplay);
    else if (keys.enter && selection == 2) openScreen(ScreenState::SettingsStorage);
    else if (keys.enter && selection == 3) openScreen(ScreenState::SettingsDevice);
    else if (keys.enter && selection == 4) openScreen(ScreenState::SettingsTrust);
  } else if (screen == ScreenState::SettingsConnectivity) {
    if (backPressed(keys)) goBack();
    else if (keys.enter) {
      beginProvisioning();
      return;
    }
  } else if (screen == ScreenState::SettingsDisplay) {
    moveSelection(keys, 9);
    if (backPressed(keys)) goBack();
    else if (keys.enter && selection == 5) {
      openScreen(ScreenState::SettingsBoot);
      return;
    }
    else if (leftPressed(keys) || rightPressed(keys)) {
      const bool forward = rightPressed(keys);
      if (selection == 0) {
        int value = static_cast<int>(uiTheme) + (forward ? 1 : kThemeCount - 1);
        uiTheme = static_cast<UiTheme>(value % kThemeCount);
        preferences.putUChar("theme", static_cast<uint8_t>(uiTheme));
      } else if (selection == 1) {
        navigationStyle = navigationStyle == NavigationStyle::Cards ?
            NavigationStyle::List : NavigationStyle::Cards;
        preferences.putUChar("nav_style", static_cast<uint8_t>(navigationStyle));
      } else if (selection == 2) {
        if (forward) displayBrightness = displayBrightness >= 224 ? 224 : displayBrightness + 32;
        else displayBrightness = displayBrightness <= 64 ? 64 : displayBrightness - 32;
        M5Cardputer.Display.setBrightness(displayBrightness);
        preferences.putUChar("brightness", displayBrightness);
      } else if (selection == 3) {
        static const uint16_t timeouts[] = {0, 30, 60, 120};
        size_t index = 0;
        while (index < 4 && timeouts[index] != screenTimeoutSeconds) ++index;
        if (index >= 4) index = 0;
        index = forward ? (index + 1) % 4 : (index + 3) % 4;
        screenTimeoutSeconds = timeouts[index];
        preferences.putUShort("timeout_s", screenTimeoutSeconds);
      } else if (selection == 4) {
        int value = static_cast<int>(idleStyle) + (forward ? 1 : 3);
        idleStyle = static_cast<IdleStyle>(value % 4);
        preferences.putUChar("idle", static_cast<uint8_t>(idleStyle));
      } else if (selection == 6) {
        searchNodesOnBoot = !searchNodesOnBoot;
        preferences.putBool("search_boot", searchNodesOnBoot);
      } else if (selection == 7) {
        interfaceSounds = !interfaceSounds;
        preferences.putBool("ui_sound", interfaceSounds);
        if (interfaceSounds) playUiCue(UiCue::Confirm);
      } else if (selection == 8) {
        if (forward) interfaceVolume = interfaceVolume >= 120 ? 24 : interfaceVolume + 24;
        else interfaceVolume = interfaceVolume <= 24 ? 120 : interfaceVolume - 24;
        preferences.putUChar("ui_volume", interfaceVolume);
        playUiCue(UiCue::Move);
      }
      lastInputMs = millis();
      fieldStatus = "Display settings saved";
    }
  } else if (screen == ScreenState::SettingsBoot) {
    moveSelection(keys, 4);
    if (backPressed(keys)) goBack();
    else if (selection == 3 && keys.enter) {
      const bool enabled = bootAnimationEnabled;
      bootAnimationEnabled = true;
      drawBootAnimation();
      bootAnimationEnabled = enabled;
      lastInputMs = millis();
    } else if (leftPressed(keys) || rightPressed(keys) || keys.enter) {
      const bool forward = !leftPressed(keys);
      if (selection == 0) {
        bootAnimationEnabled = !bootAnimationEnabled;
        preferences.putBool("boot_anim", bootAnimationEnabled);
      } else if (selection == 1) {
        int value = static_cast<int>(bootSequence) + (forward ? 1 : 5);
        bootSequence = static_cast<BootSequence>(value % 6);
        preferences.putUChar("boot_seq", static_cast<uint8_t>(bootSequence));
      } else if (selection == 2) {
        int value = static_cast<int>(bootSpeed) + (forward ? 1 : 2);
        bootSpeed = static_cast<BootSpeed>(value % 3);
        preferences.putUChar("boot_speed", static_cast<uint8_t>(bootSpeed));
      }
    }
  } else if (screen == ScreenState::SettingsStorage) {
    if (backPressed(keys)) goBack();
    else if (keys.enter) mountEvidence();
  } else if (screen == ScreenState::SettingsDevice) {
    moveSelection(keys, 2);
    if (backPressed(keys)) goBack();
    else if (selection == 0 && (leftPressed(keys) || rightPressed(keys) || keys.enter)) {
      const int direction = leftPressed(keys) ? 2 : 1;
      participationMode = static_cast<ParticipationMode>(
          (static_cast<int>(participationMode) + direction) % 3);
      preferences.putUChar("participate", static_cast<uint8_t>(participationMode));
      fieldStatus = "Participation saved";
    } else if (selection == 1 && keys.enter) {
      systemOpenedFromSettings = true;
      openScreen(ScreenState::System);
    }
  } else if (screen == ScreenState::SettingsTrust) {
    moveSelection(keys, 3);
    if (backPressed(keys)) goBack();
    else if (keys.enter && selection == 0 && peerKeyValid) {
      openScreen(ScreenState::ConfirmForgetTrust);
    } else if (keys.enter && selection > 0) {
      const bool execution = selection == 1;
      bool& valid = execution ? executionKeyValid : evidenceKeyValid;
      uint8_t* key = execution ? executionKey : evidenceKey;
      const char* preference = execution ? "exec_key" : "evid_key";
      if (valid) {
        memset(key, 0, kPeerKeyBytes);
        valid = false;
        preferences.remove(preference);
        notice = execution ? "Execution key removed" : "Evidence key removed";
        draw();
      } else {
        input = "";
        provisioningExecutionKey = execution;
        openScreen(ScreenState::ProvisionEvidenceKey);
      }
    } else draw();
  } else if (screen == ScreenState::ConfirmForgetTrust) {
    if (backPressed(keys)) goBack();
    else if (keys.enter) {
      memset(peerKey, 0, sizeof(peerKey));
      peerKeyValid = false;
      trustedP4Id = "";
      preferences.remove("peer_key");
      preferences.remove("peer_p4");
      pairingStatus = "Local trust removed; reset P4 trust";
      notice = "Local pairing removed";
      openScreen(ScreenState::SettingsTrust);
      return;
    }
  }
  draw();
}

void handleProvisionInput(const Keyboard_Class::KeysState& keys) {
  if (keys.esc) {
    if (!preferences.getString("ssid", "").isEmpty()) {
      wifiSsid = preferences.getString("ssid", "");
      wifiPassword = preferences.getString("password", "");
      startConnecting();
    } else workOffline();
    return;
  }
  if (keys.backspace && !input.isEmpty()) input.remove(input.length() - 1);
  for (const char value : keys.word) {
    if (value >= 32 && value <= 126 &&
        input.length() < (screen == ScreenState::ProvisionSsid ? 32U : 63U)) input += value;
  }
  if (keys.enter && (screen == ScreenState::ProvisionPassword || !input.isEmpty())) {
    if (screen == ScreenState::ProvisionSsid) {
      wifiSsid = input;
      input = "";
      screen = ScreenState::ProvisionPassword;
    } else {
      wifiPassword = input;
      preferences.putString("ssid", wifiSsid);
      preferences.putString("password", wifiPassword);
      input = "";
      startConnecting();
      return;
    }
  }
  draw();
}

void handleEvidenceKeyInput(const Keyboard_Class::KeysState& keys) {
  if (keys.esc) {
    input = "";
    goBack();
    return;
  }
  if (keys.backspace && !input.isEmpty()) input.remove(input.length() - 1);
  for (const char value : keys.word) {
    if (value >= 32 && value <= 126 && input.length() < 63) input += value;
  }
  if (keys.enter && !input.isEmpty()) {
    uint8_t* key = provisioningExecutionKey ? executionKey : evidenceKey;
    sha256Digest(input, key);
    if (provisioningExecutionKey) executionKeyValid = true;
    else evidenceKeyValid = true;
    preferences.putBytes(provisioningExecutionKey ? "exec_key" : "evid_key", key, kPeerKeyBytes);
    input = "";
    notice = provisioningExecutionKey ? "Execution key saved" : "Evidence key saved";
    returnToMenu(ScreenState::SettingsTrust, provisioningExecutionKey ? 1 : 2);
    return;
  }
  draw();
}

void handleNfcContentInput(const Keyboard_Class::KeysState& keys) {
  if (keys.tab) {
    if (loadNfcPresets()) openScreen(ScreenState::NfcPresets);
    else { fieldStatus = "microSD unavailable"; draw(); }
    return;
  }
  if (keys.esc) { goBack(); return; }
  const String before = input;
  if (keys.backspace && !input.isEmpty()) input.remove(input.length() - 1);
  for (const char c : keys.word) {
    if (c >= 32 && c <= 126 && input.length() < 120) input += c;
  }
  if (input != before) fieldStatus = "";
  if (keys.enter && input.isEmpty()) fieldStatus = "Enter content before continuing";
  if (keys.enter && !input.isEmpty()) {
    nfcPayload = input;
    if (nfcAction < 2) {
      if (cap::prepareNdefWrite(nfcPayload, nfcContentIsUrl())) {
        openScreen(ScreenState::NfcWriteConfirm);
        return;
      }
    } else if (wifiScanning || bleScanning || localHostScan.active() || remoteHostScan.active() || portScan.active()) {
      fieldStatus = "Finish running scans first";
      draw();
      return;
    } else if (cap::startEmulation(nfcPayload, nfcContentIsUrl(), nfcAction >= 4)) {
      openScreen(ScreenState::NfcEmulation);
      return;
    }
    fieldStatus = cap::status();
  }
  draw();
}

void handleProjectInput(const Keyboard_Class::KeysState& keys) {
  if (keys.esc) {
    input = "";
    returnToScreen(ScreenState::Projects);
    return;
  }
  if (keys.backspace && !input.isEmpty()) input.remove(input.length() - 1);
  for (const char value : keys.word) {
    if ((isalnum(static_cast<unsigned char>(value)) || value == '-' || value == '_' ||
         value == '.') && input.length() < 32) input += value;
  }
  if (keys.enter && !input.isEmpty()) {
    const String projectId = input;
    input = "";
    if (createProject(projectId)) {
      playUiCue(UiCue::Confirm);
      selection = 0;
      for (size_t index = 0; index < projects.size(); ++index)
        if (projects[index].projectId == projectId) selection = index;
      returnToScreen(ScreenState::Projects);
    } else {
      fieldStatus = "Project exists or could not be saved";
      playUiCue(UiCue::Warning);
    }
  }
  draw();
}

}  // namespace

void setup() {
  auto config = M5.config();
  config.output_power = true;
  M5Cardputer.begin(config, true);
  M5Cardputer.Speaker.begin();
  uiCanvas.setColorDepth(16);
  uiCanvasReady = uiCanvas.createSprite(M5Cardputer.Display.width(),
                                        M5Cardputer.Display.height()) != nullptr;
  M5Cardputer.Display.setBrightness(160);
  Serial.begin(115200);
  preferences.begin("reconclave", false);
  const String disabledPolicy = preferences.getString("cap_off", "");
  size_t policyStart = 0;
  while (policyStart < disabledPolicy.length()) {
    int separator = disabledPolicy.indexOf('\n', policyStart);
    if (separator < 0) separator = disabledPolicy.length();
    const String key = disabledPolicy.substring(policyStart, separator);
    if (!key.isEmpty()) disabledCapabilities.push_back(key);
    policyStart = static_cast<size_t>(separator) + 1;
  }
  displayBrightness = preferences.getUChar("brightness", 160);
  if (displayBrightness < 64 || displayBrightness > 224) displayBrightness = 160;
  const uint8_t savedTheme = preferences.getUChar("theme", 0);
  uiTheme = savedTheme < kThemeCount ? static_cast<UiTheme>(savedTheme) : UiTheme::Field;
  const uint8_t savedNavigation = preferences.getUChar("nav_style", 0);
  navigationStyle = savedNavigation <= 1 ? static_cast<NavigationStyle>(savedNavigation) :
      NavigationStyle::Cards;
  const uint8_t savedIdle = preferences.getUChar("idle", 1);
  idleStyle = savedIdle <= 3 ? static_cast<IdleStyle>(savedIdle) : IdleStyle::Radar;
  screenTimeoutSeconds = preferences.getUShort("timeout_s", 60);
  if (screenTimeoutSeconds != 0 && screenTimeoutSeconds != 30 &&
      screenTimeoutSeconds != 60 && screenTimeoutSeconds != 120) screenTimeoutSeconds = 60;
  bootAnimationEnabled = preferences.getBool("boot_anim", true);
  searchNodesOnBoot = preferences.getBool("search_boot", false);
  interfaceSounds = preferences.getBool("ui_sound", true);
  interfaceVolume = preferences.getUChar("ui_volume", 72);
  if (interfaceVolume < 24 || interfaceVolume > 120) interfaceVolume = 72;
  const uint8_t savedBootSequence = preferences.getUChar("boot_seq", 0);
  bootSequence = savedBootSequence <= 5 ? static_cast<BootSequence>(savedBootSequence) :
      BootSequence::CipherRain;
  const uint8_t savedBootSpeed = preferences.getUChar("boot_speed", 1);
  bootSpeed = savedBootSpeed <= 2 ? static_cast<BootSpeed>(savedBootSpeed) : BootSpeed::Normal;
  const uint8_t savedParticipation = preferences.getUChar("participate", 2);
  participationMode = savedParticipation <= 2 ? static_cast<ParticipationMode>(savedParticipation) :
      ParticipationMode::Both;
  M5Cardputer.Display.setBrightness(displayBrightness);
  drawBootAnimation();
  lastInputMs = millis();
  const uint64_t chip = ESP.getEfuseMac();
  char id[32];
  // ESP.getEfuseMac() exposes the six MAC bytes in little-endian integer order.
  // Render the network/hardware order so the runtime identity matches the ID
  // printed by the flasher and used during offline provisioning.
  snprintf(id, sizeof(id), "rc-adv-%02x%02x%02x%02x%02x%02x",
           static_cast<unsigned int>((chip >> 0) & 0xff),
           static_cast<unsigned int>((chip >> 8) & 0xff),
           static_cast<unsigned int>((chip >> 16) & 0xff),
           static_cast<unsigned int>((chip >> 24) & 0xff),
           static_cast<unsigned int>((chip >> 32) & 0xff),
           static_cast<unsigned int>((chip >> 40) & 0xff));
  deviceId = id;
  if (preferences.getBytesLength("peer_key") == sizeof(peerKey) &&
      preferences.getBytes("peer_key", peerKey, sizeof(peerKey)) == sizeof(peerKey)) {
    trustedP4Id = preferences.getString("peer_p4", "");
    peerKeyValid = !trustedP4Id.isEmpty();
  }
  evidenceKeyValid = preferences.getBytesLength("evid_key") == sizeof(evidenceKey) &&
      preferences.getBytes("evid_key", evidenceKey, sizeof(evidenceKey)) == sizeof(evidenceKey);
  executionKeyValid = preferences.getBytesLength("exec_key") == sizeof(executionKey) &&
      preferences.getBytes("exec_key", executionKey, sizeof(executionKey)) == sizeof(executionKey);
  // Build-time provisioning is authoritative for this fleet generation. Each
  // relationship has a distinct key; no fleet-wide command secret exists.
  trustedP4Id = RC_PROVISIONED_P4_ID;
  memcpy(peerKey, RC_PROVISIONED_P4_KEY, sizeof(peerKey));
  peerKeyValid = true;
  memcpy(executionKey, RC_PROVISIONED_PRIMARY_KEY, sizeof(executionKey));
  executionKeyValid = true;
  uint8_t nodeBootNonce[16];
  esp_fill_random(nodeBootNonce, sizeof(nodeBootNonce));
  hexEncode(nodeBootNonceHex, nodeBootNonce, sizeof(nodeBootNonce));
  groveSerial.setRxBufferSize(1024);
  groveSerial.begin(115200, SERIAL_8N1, kGroveRxPin, kGroveTxPin);
  mountEvidence();
  loadEvidenceCollectors();
  loadRemoteTask();
  wifiSsid = preferences.getString("ssid", "");
  wifiPassword = preferences.getString("password", "");
  if (wifiSsid.isEmpty()) beginProvisioning();
  else startConnecting();
}

void loop() {
  M5Cardputer.update();
  // NFC target replies have tight deadlines. Keep emulation exclusive on the
  // UI task; don't interleave HTTP waits, SD writes, scans or idle animations.
  if (cap::emulating()) {
    if (cap::update()) draw();
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
      lastInputMs = millis();
      handleApplicationInput(M5Cardputer.Keyboard.keysState());
    }
    delay(1);
    return;
  }
  updateObservationScans();
  if (cap::update() && (screen == ScreenState::SubGhz || screen == ScreenState::NfcResults)) draw();
  updateGrove();
  if (serverReady) server.handleClient();
  // A successful fleet.ota.apply upload sets this rather than calling esp_restart()
  // directly, so the HTTP response confirming success has already been flushed to the
  // coordinator by the time handleClient() returns control here.
  if (pendingRebootAtMs != 0 && (int32_t)(millis() - pendingRebootAtMs) >= 0) {
    esp_restart();
  }
  if (millis() - lastOutboxRetryMs >= 30000UL) {
    lastOutboxRetryMs = millis();
    retryEvidenceOutbox();
  }

  if (wifiRestorePending && WiFi.status() == WL_CONNECTED) {
    wifiRestorePending = false;
    if (mdnsReady) MDNS.end();
    mdnsReady = false;
    startNodeServices();
    fieldStatus += "; Wi-Fi restored";
    draw();
  }

  if (screen == ScreenState::Connecting) {
    if (WiFi.status() == WL_CONNECTED) {
      startNodeServices();
      screen = ScreenState::Home;
      notice = "Connected";
      draw();
      if (searchNodesOnBoot) discover();
      screen = ScreenState::Home;
      draw();
    } else if (millis() - connectStartedMs > kConnectTimeoutMs) {
      notice = "Connection failed; W to change";
      draw();
      connectStartedMs = millis();
    }
  }

  const size_t nodeCountBeforeExpiry = remoteNodes.size();
  remoteNodes.erase(std::remove_if(remoteNodes.begin(), remoteNodes.end(),
      [](const RemoteNode& node) { return millis() - node.lastSeenMs > kNodeExpiryMs; }),
      remoteNodes.end());
  if (!p4.deviceId.isEmpty() && millis() - p4.lastSeenMs > kNodeExpiryMs) {
    registry.expireBefore(static_cast<uint64_t>(millis() - kNodeExpiryMs) + 1);
    p4 = {};
  }
  if (remoteNodes.size() != nodeCountBeforeExpiry &&
      (screen == ScreenState::Reconclave || screen == ScreenState::NodeDetail)) {
    notice = "Offline nodes removed";
    draw();
  }

  localHostScan.update();
  LocalHostResult localResult;
  bool localChanged = false;
  while (localHostScan.nextResult(localResult)) {
    const String ip = localResult.ip.toString();
    discoveredHosts.push_back(ip);
    localScoutHosts.push_back(ip);
    localChanged = true;
  }
  // Not gated on scoutRunning itself: that flag is only recomputed inside
  // mergeScoutProgress, so a purely-local recurring job (which genuinely goes idle
  // between passes) would otherwise never trigger another merge once it first went
  // idle, even after the reschedule below starts a new pass.
  if ((localScoutAssigned || !remoteScoutJobs.empty()) && millis() - lastScoutUiMs >= 250) {
    lastScoutUiMs = millis();
    mergeScoutProgress();
    localChanged = true;
  }
  if (localScoutAssigned && !localHostScan.active() && scoutIntervalMinutes > 0) {
    if (localScoutNextRunMs == 0) {
      // This local pass just finished; ship its results and schedule the next one.
      ++scoutRunCount;
      distributeScoutEvidence(localScoutHosts, deviceId);
      detectAndRecordChanges(localScoutHosts, deviceId);
      localScoutNextRunMs = millis() + static_cast<unsigned long>(scoutIntervalMinutes) * 60000UL;
    } else if (!bleScanning && reconclave::uptimeDue(millis(), localScoutNextRunMs) &&
               localHostScan.startRange(localScoutFirstHost,
                   localScoutLastHost - localScoutFirstHost + 1)) {
      localScoutHosts.clear();
      localScoutNextRunMs = 0;
      localChanged = true;
    }
  }
  if (localChanged && screen == ScreenState::Scout) draw();

  remoteHostScan.update();
  LocalHostResult remoteResult;
  while (remoteHostScan.nextResult(remoteResult)) {
    const String host = remoteResult.ip.toString();
    if (std::find(remoteNodeScanHosts.begin(), remoteNodeScanHosts.end(), host) ==
        remoteNodeScanHosts.end()) remoteNodeScanHosts.push_back(host);
  }
  if (remoteNodeJobWasRunning && !remoteHostScan.active()) {
    remoteNodeJobWasRunning = false;
    ++remoteNodeJobRunCount;
    saveRemoteTask();
    const String runId = remoteNodeJobId + "-" + String(remoteNodeJobRunCount);
    for (const auto& host : remoteNodeScanHosts) {
      JsonDocument evidenceDoc;
      JsonObject evidence = evidenceDoc.to<JsonObject>();
      evidence["evidence_id"] = deviceId + "-" + runId + "-" + host;
      evidence["job_id"] = remoteNodeJobId;
      evidence["source_node"] = deviceId;
      evidence["target"] = host;
      evidence["timestamp_ms"] = static_cast<uint64_t>(millis()) + 1;
      evidence["method"] = "net.discovery.scan";
      if (!remoteNodeJobProject.isEmpty()) {
        evidence["project_id"] = remoteNodeJobProject;
        evidence["scope_id"] = "default";
        evidence["scope_revision"] = remoteNodeJobScopeRevision;
      }
      evidence["observation"].to<JsonObject>()["responsive"] = true;
      String storeError;
      writeEvidenceRecord(evidence, storeError);
      for (const auto& node : evidenceCollectors) {
        if (nodeHasCapability(node, "storage.evidence.write") && evidenceKeyValid &&
            !sendEvidenceToNode(node, evidence)) queueEvidenceForNode(node, evidence);
      }
    }
    if (remoteNodeJobRecurring && !remoteNodeJobCancelled) {
      if (remoteNodeJobCallback) {
        remoteNodeCallbackNextAttemptMs = std::max(1UL, millis());
        remoteNodeJobNextRunMs = 0;
      } else {
        remoteNodeJobNextRunMs = millis() + remoteNodeJobIntervalMs;
      }
      saveRemoteTask();
    }
  }
  if (remoteNodeJobRecurring && remoteNodeJobCallback && !remoteNodeJobCancelled &&
      remoteNodeCallbackNextAttemptMs != 0 &&
      static_cast<int32_t>(millis() - remoteNodeCallbackNextAttemptMs) >= 0) {
    if (callbackCoordinatorReachable()) {
      remoteNodeCallbackFailures = 0;
      remoteNodeCallbackNextAttemptMs = 0;
      remoteNodeJobNextRunMs = millis() + remoteNodeJobIntervalMs;
    } else if (++remoteNodeCallbackFailures >= 3) {
      remoteNodeJobCancelled = true;
      remoteNodeJobRecurring = false;
      remoteNodeCallbackNextAttemptMs = 0;
    } else {
      remoteNodeCallbackNextAttemptMs = millis() +
          (5000UL << (remoteNodeCallbackFailures - 1));
    }
    saveRemoteTask();
  }
  if (remoteNodeJobRecurring && !remoteNodeJobCancelled && remoteNodeJobNextRunMs != 0 &&
      static_cast<int32_t>(millis() - remoteNodeJobNextRunMs) >= 0 && evidenceStorageAvailable()) {
    if (remoteHostScan.startRange(remoteNodeFirstHost,
                                  remoteNodeLastHost - remoteNodeFirstHost + 1)) {
      remoteNodeScanHosts.clear();
      remoteNodeJobNextRunMs = 0;
      remoteNodeJobWasRunning = true;
    }
  }

  portScan.update();
  NetworkPortResult portResult;
  bool portChanged = false;
  while (portScan.nextResult(portResult)) {
    openPorts.push_back(portResult.port);
    portChanged = true;
  }
  if (portRunning) {
    if (!portScan.active()) {
      portRunning = false;
      portStatus = String(portScan.checked()) + "/" + String(portScan.total()) +
          " | " + String(openPorts.size()) + " open";
      portChanged = true;
    } else if (millis() - lastScoutPollMs >= 150) {
      lastScoutPollMs = millis();
      portStatus = String(portScan.checked()) + "/" + String(portScan.total());
      portChanged = true;
    }
  }
  if (portChanged && screen == ScreenState::PortResults) draw();

  if (scoutRunning && !scoutRemoteNodeId.isEmpty() &&
      millis() - lastScoutPollMs >= 750) {
    lastScoutPollMs = millis();
    for (auto& job : remoteScoutJobs) if (job.running) requestScoutUpdate(job, false);
    mergeScoutProgress();
    if (screen == ScreenState::Scout) draw();
  }

  if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
    const auto keys = M5Cardputer.Keyboard.keysState();
    lastInputMs = millis();
    if (idleActive) {
      idleActive = false;
      M5Cardputer.Display.setBrightness(displayBrightness);
      draw();
    } else if (screen == ScreenState::ProvisionSsid || screen == ScreenState::ProvisionPassword) {
      handleProvisionInput(keys);
    } else if (screen == ScreenState::ProvisionEvidenceKey) {
      handleEvidenceKeyInput(keys);
    } else if (screen == ScreenState::NfcContent) {
      handleNfcContentInput(keys);
    } else if (screen == ScreenState::ProvisionProject) {
      handleProjectInput(keys);
    } else handleApplicationInput(keys);
  }
  const bool canIdle = screen != ScreenState::ProvisionSsid &&
      screen != ScreenState::ProvisionPassword && screen != ScreenState::Connecting &&
      screen != ScreenState::ProvisionEvidenceKey && screen != ScreenState::ProvisionProject &&
      screen != ScreenState::NfcContent && screen != ScreenState::NfcWriteConfirm &&
      screen != ScreenState::NfcPresets && screen != ScreenState::NfcViewer;
  if (!idleActive && canIdle && screenTimeoutSeconds > 0 &&
      millis() - lastInputMs >= static_cast<unsigned long>(screenTimeoutSeconds) * 1000UL) {
    idleActive = true;
    idlePhase = 0;
    lastIdleFrameMs = millis();
    draw();
  } else if (idleActive && idleStyle != IdleStyle::Off &&
             millis() - lastIdleFrameMs >= 250) {
    lastIdleFrameMs = millis();
    idlePhase += 5;
    draw();
  }
  delay(10);
}
