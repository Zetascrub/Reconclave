// K230 native app - milestone 2 (network proof).
//
// Announces on `_reconclave._tcp.local.` and serves GET
// /reconclave/v1/announce + POST /reconclave/v1/message over plain HTTP,
// answering read-only `system.info` and `net.arp.snapshot` capabilities. Matches the transport
// tools/desktop-node/reconclave_node.py actually implements (see
// devices/k230/README.md for why that is HTTP, not the WebSocket the docs
// describe). No display or camera work yet - see the milestone list in
// devices/k230/README.md.

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>

#include "reconclave/protocol.h"
#include "src/http_server.h"
#include "src/json.h"
#include "src/mdns_responder.h"
#include "src/network_inventory.h"
#include "src/network_tools.h"
#include "src/trust_policy.h"
#include "src/discovery_job.h"
#include "src/evidence_store.h"
#include "src/host_knowledge.h"
#include "src/wifi_scanner.h"

namespace {

constexpr char kDeviceId[] = "rc-k230-poc";
constexpr char kFirmware[] = "reconclave-k230-0.2.0-alpha4";
constexpr char kServiceType[] = "_reconclave._tcp";
constexpr std::uint16_t kHttpPort = 8787;
constexpr char kAnnouncePath[] = "/reconclave/v1/announce";
constexpr char kMessagePath[] = "/reconclave/v1/message";
constexpr char kRuntimeStatusPath[] = "/run/reconclave/node-status";
constexpr char kEvidenceDir[] = "/root/reconclave/evidence";
constexpr char kSessionPath[] = "/root/reconclave/session.conf";
constexpr char kHostKnowledgePath[] = "/root/reconclave/hosts.json";
constexpr char kLocalCommandPath[] = "/run/reconclave/local-command";

volatile std::sig_atomic_t g_running = 1;
void handleSignal(int) { g_running = 0; }

std::uint64_t nowMillis() {
  struct timespec ts{};
  clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000 + static_cast<std::uint64_t>(ts.tv_nsec) / 1000000;
}

double monotonicSeconds() {
  struct timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

// First non-loopback IPv4 address on an UP interface (this device is
// single-homed in testing - eth0 only - so the first match is sufficient;
// a multi-homed deployment would need an explicit interface argument).
std::string findLocalIpv4() {
  ifaddrs* addrs = nullptr;
  if (getifaddrs(&addrs) != 0) return "";
  std::string result;
  for (ifaddrs* it = addrs; it != nullptr; it = it->ifa_next) {
    if (it->ifa_addr == nullptr || it->ifa_addr->sa_family != AF_INET) continue;
    if ((it->ifa_flags & IFF_LOOPBACK) != 0) continue;
    if ((it->ifa_flags & IFF_UP) == 0) continue;
    char buf[INET_ADDRSTRLEN];
    auto* sin = reinterpret_cast<sockaddr_in*>(it->ifa_addr);
    if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf)) != nullptr) {
      result = buf;
      break;
    }
  }
  freeifaddrs(addrs);
  return result;
}

reconclave::json::Value announcementToJson(const reconclave::NodeAnnouncement& announcement,
                                            const reconclave::TrustPolicy& trust) {
  using reconclave::json::Value;
  Value payload = Value::makeObject();
  payload.set("device_id", Value::makeString(announcement.device_id));
  payload.set("device_type", Value::makeString(announcement.device_type));
  payload.set("firmware", Value::makeString(announcement.firmware));

  Value roles = Value::makeArray();
  for (const auto& role : announcement.roles) roles.push_back(Value::makeString(role));
  payload.set("roles", roles);

  Value capabilities = Value::makeArray();
  for (const auto& capability : announcement.capabilities) {
    capabilities.push_back(Value::makeString(capability));
  }
  payload.set("capabilities", capabilities);

  Value descriptors = Value::makeArray();
  for (const auto& descriptor : announcement.capability_descriptors) {
    Value entry = Value::makeObject();
    entry.set("id", Value::makeString(descriptor.id));
    entry.set("version", Value::makeNumber(descriptor.version));
    entry.set("permission", Value::makeString(descriptor.permission));
    Value features = Value::makeArray();
    for (const auto& feature : descriptor.features) features.push_back(Value::makeString(feature));
    entry.set("features", features);
    entry.set("weight", Value::makeNumber(descriptor.weight));
    entry.set("max_concurrency", Value::makeNumber(descriptor.max_concurrency));
    descriptors.push_back(entry);
  }
  payload.set("capability_descriptors", descriptors);

  Value resources = Value::makeObject();
  resources.set("network_mbps", Value::makeNumber(announcement.resources.network_mbps));
  resources.set("persistent_storage", Value::makeBool(announcement.resources.persistent_storage));
  resources.set("storage_free_bytes", Value::makeNumber(
                                           static_cast<double>(announcement.resources.storage_free_bytes)));
  payload.set("resources", resources);
  payload.set("status", Value::makeString(announcement.status));
  Value security = Value::makeObject();
  security.set("boot_nonce", Value::makeString(trust.bootNonce()));
  security.set("mode", Value::makeString(trust.configured() ? "hmac-sha256-128" : "unprovisioned"));
  payload.set("security", security);
  return payload;
}

reconclave::json::Value makeEnvelope(const std::string& type, const std::string& destination,
                                      std::uint64_t sequence) {
  using reconclave::json::Value;
  Value envelope = Value::makeObject();
  envelope.set("proto", Value::makeString(reconclave::kProtocolVersion));
  envelope.set("type", Value::makeString(type));
  envelope.set("message_id", Value::makeString(kDeviceId + std::string("-") + std::to_string(sequence)));
  envelope.set("source_node", Value::makeString(kDeviceId));
  if (!destination.empty()) envelope.set("destination_node", Value::makeString(destination));
  envelope.set("timestamp_ms", Value::makeNumber(static_cast<double>(nowMillis())));
  envelope.set("sequence", Value::makeNumber(static_cast<double>(sequence)));
  return envelope;
}

reconclave::json::Value systemInfo(double started_at) {
  using reconclave::json::Value;
  utsname uts{};
  uname(&uts);
  Value result = Value::makeObject();
  result.set("device_type", Value::makeString("k230"));
  result.set("firmware", Value::makeString(kFirmware));
  result.set("platform", Value::makeString(std::string(uts.sysname) + " " + uts.release));
  result.set("hostname", Value::makeString(uts.nodename));
  result.set("uptime_ms", Value::makeNumber(
                               std::floor((monotonicSeconds() - started_at) * 1000.0)));
  return result;
}

reconclave::json::Value sshInfo() {
  using reconclave::json::Value;
  bool server_running = false;
  std::ifstream pid_file("/var/run/sshd.pid");
  long pid = 0;
  pid_file >> pid;
  if (pid > 1) server_running = kill(static_cast<pid_t>(pid), 0) == 0;
  Value result = Value::makeObject();
  result.set("client_available", Value::makeBool(access("/usr/bin/ssh", X_OK) == 0));
  result.set("server_running", Value::makeBool(server_running));
  result.set("port", Value::makeNumber(22));
  result.set("authentication", Value::makeString("public-key-only"));
  result.set("password_authentication", Value::makeBool(false));
  result.set("tcp_forwarding", Value::makeBool(false));
  result.set("sftp_available", Value::makeBool(access("/usr/bin/sftp", X_OK) == 0));
  result.set("scp_available", Value::makeBool(access("/usr/bin/scp", X_OK) == 0));
  result.set("client_identity_ready", Value::makeBool(
      access("/root/.ssh/id_ed25519", R_OK) == 0 &&
      access("/root/.ssh/id_ed25519.pub", R_OK) == 0));
  std::ifstream public_key("/root/.ssh/id_ed25519.pub");
  std::string public_identity;
  std::getline(public_key, public_identity);
  if (!public_identity.empty()) result.set("client_public_key", Value::makeString(public_identity));
  return result;
}

reconclave::json::Value arpSnapshot() {
  using reconclave::json::Value;
  std::vector<reconclave::ArpNeighbour> neighbours;
  std::string error;
  Value result = Value::makeObject();
  Value entries = Value::makeArray();
  const bool ok = reconclave::readArpSnapshot(neighbours, error);
  if (ok) {
    for (const auto& neighbour : neighbours) {
      Value entry = Value::makeObject();
      entry.set("address", Value::makeString(neighbour.address));
      entry.set("mac", Value::makeString(neighbour.mac));
      entry.set("interface", Value::makeString(neighbour.interface));
      entry.set("complete", Value::makeBool(neighbour.complete));
      entries.push_back(entry);
    }
  }
  result.set("neighbours", entries);
  result.set("count", Value::makeNumber(static_cast<double>(neighbours.size())));
  result.set("observer_node", Value::makeString(kDeviceId));
  if (!ok) result.set("error", Value::makeString(error));
  return result;
}

reconclave::json::Value dnsLookup(const reconclave::json::Value& arguments) {
  using reconclave::json::Value;
  Value result = Value::makeObject();
  const auto* hostname = arguments.find("hostname");
  reconclave::DnsResult resolved;
  std::string error;
  const bool ok = hostname && reconclave::resolveHost(hostname->asString(), resolved, error);
  result.set("hostname", Value::makeString(hostname ? hostname->asString() : ""));
  result.set("canonical_name", Value::makeString(resolved.canonical_name));
  Value addresses = Value::makeArray();
  for (const auto& address : resolved.addresses) addresses.push_back(Value::makeString(address));
  result.set("addresses", addresses);
  result.set("success", Value::makeBool(ok));
  if (!ok) result.set("error", Value::makeString(error));
  return result;
}

reconclave::json::Value tcpCheck(const reconclave::json::Value& arguments) {
  using reconclave::json::Value;
  const auto* target = arguments.find("target");
  const auto* port = arguments.find("port");
  const auto* timeout = arguments.find("timeout_ms");
  reconclave::TcpObservation observed;
  const bool ok = target && port && reconclave::tcpConnect(
      target->asString(), static_cast<std::uint16_t>(port->asUInt64()),
      static_cast<int>(timeout ? timeout->asUInt64(1000) : 1000), observed);
  Value result = Value::makeObject();
  result.set("target", Value::makeString(observed.address));
  result.set("port", Value::makeNumber(observed.port));
  result.set("connected", Value::makeBool(ok));
  result.set("latency_ms", Value::makeNumber(observed.latency_ms));
  if (!ok) result.set("error", Value::makeString(observed.error));
  return result;
}

reconclave::json::Value serviceIdentify(const reconclave::json::Value& arguments,
                                        reconclave::ServiceObservation* captured = nullptr) {
  using reconclave::json::Value;
  const auto* target = arguments.find("target");
  const auto* port = arguments.find("port");
  const auto* timeout = arguments.find("timeout_ms");
  reconclave::ServiceObservation observed;
  const bool ok = target && port && reconclave::identifyService(
      target->asString(), static_cast<std::uint16_t>(port->asUInt64()),
      static_cast<int>(timeout ? timeout->asUInt64(1000) : 1000), observed);
  if (captured != nullptr) *captured = observed;
  Value result = Value::makeObject();
  result.set("target", Value::makeString(observed.address));
  result.set("port", Value::makeNumber(observed.port));
  result.set("connected", Value::makeBool(ok));
  result.set("transport", Value::makeString(observed.transport));
  result.set("service", Value::makeString(observed.service));
  result.set("banner", Value::makeString(observed.banner));
  result.set("attention", Value::makeString(observed.attention));
  if (!ok) result.set("error", Value::makeString(observed.error));
  return result;
}

reconclave::json::Value wifiSurvey() {
  using reconclave::json::Value;
  std::vector<reconclave::WifiObservation> observations;
  std::string error;
  const bool ok = reconclave::scanWifi("wlan0", observations, error);
  const auto summary = reconclave::analyseWifi(observations);
  Value result = Value::makeObject();
  result.set("success", Value::makeBool(ok));
  result.set("count", Value::makeNumber(summary.total));
  result.set("open_count", Value::makeNumber(summary.open));
  result.set("hidden_count", Value::makeNumber(summary.hidden));
  result.set("best_24_channel", Value::makeNumber(summary.best_24_channel));
  Value access_points = Value::makeArray();
  for (const auto& ap : observations) {
    Value entry = Value::makeObject();
    entry.set("ssid", Value::makeString(ap.ssid));
    entry.set("bssid", Value::makeString(ap.bssid));
    entry.set("channel", Value::makeNumber(ap.channel));
    entry.set("frequency_mhz", Value::makeNumber(ap.frequency_mhz));
    entry.set("rssi_dbm", Value::makeNumber(ap.rssi_dbm));
    entry.set("quality_percent", Value::makeNumber(ap.quality_percent));
    entry.set("security", Value::makeString(ap.security));
    access_points.push_back(entry);
  }
  result.set("access_points", access_points);
  if (!ok) result.set("error", Value::makeString(error));
  return result;
}

reconclave::json::Value discoverySnapshot(const reconclave::DiscoverySnapshot& snapshot) {
  using reconclave::json::Value;
  Value result = Value::makeObject();
  result.set("job_id", Value::makeString(snapshot.job_id));
  result.set("job_status", Value::makeString(snapshot.status));
  result.set("checked", Value::makeNumber(snapshot.checked));
  result.set("total", Value::makeNumber(snapshot.total));
  Value hosts = Value::makeArray();
  for (const auto& host : snapshot.hosts) {
    Value entry = Value::makeObject();
    entry.set("address", Value::makeString(host.address));
    Value ports = Value::makeArray();
    for (const auto port : host.open_ports) ports.push_back(Value::makeNumber(port));
    entry.set("open_ports", ports);
    hosts.push_back(entry);
  }
  result.set("hosts", hosts);
  if (!snapshot.error.empty()) result.set("error", Value::makeString(snapshot.error));
  return result;
}

void writeRuntimeStatus(const reconclave::TrustPolicy& trust,
                        const reconclave::NodeAnnouncement& announcement,
                        const reconclave::DiscoverySnapshot& job,
                        const std::string& address, std::size_t known_hosts) {
  mkdir("/run/reconclave", 0755);
  const std::string temporary = std::string(kRuntimeStatusPath) + ".tmp";
  std::ofstream output(temporary, std::ios::trunc);
  if (!output) return;
  output << "ROLE             node\n"
         << "DEVICE           " << kDeviceId << "\n"
         << "TRUST            " << (trust.configured() ? "provisioned" : "unprovisioned") << "\n"
         << "ADDRESS          " << address << ":" << kHttpPort << "\n"
         << "CAPABILITIES     " << announcement.capabilities.size() << "\n"
         << "JOB              " << (job.job_id.empty() ? "none" : job.job_id) << "\n"
         << "JOB STATUS       " << job.status << "\n"
         << "PROGRESS         " << job.checked << "/" << job.total << "\n"
         << "HOSTS FOUND      " << job.hosts.size() << "\n";
  output << "KNOWN HOSTS      " << known_hosts << "\n";
  output.close();
  if (output.good()) rename(temporary.c_str(), kRuntimeStatusPath);
}

}  // namespace

int main() {
  std::signal(SIGINT, handleSignal);
  std::signal(SIGTERM, handleSignal);

  reconclave::TrustPolicy trust(kDeviceId);
  reconclave::DiscoveryJob discovery;
  reconclave::EvidenceStore evidence(kEvidenceDir, kDeviceId);
  reconclave::HostKnowledge host_knowledge(kHostKnowledgePath);
  std::string trust_error;
  std::string host_error;
  if (!host_knowledge.load(host_error)) {
    std::fprintf(stderr, "host knowledge load failed: %s\n", host_error.c_str());
  }
  const char* configured_key_path = std::getenv("RECONCLAVE_EXECUTION_KEY_FILE");
  trust.loadExecutionKey(configured_key_path ? configured_key_path :
                          "/root/reconclave/trust/execution.key", trust_error);

  reconclave::NodeAnnouncement announcement;
  announcement.device_id = kDeviceId;
  announcement.device_type = "k230";
  announcement.firmware = kFirmware;
  announcement.roles = {"node"};
  // Assessment capabilities are advertised only after execution-key
  // provisioning. Target-bearing tools additionally require delegated scope.
  announcement.capabilities = {"system.info", "system.ssh.status", "net.arp.snapshot"};
  if (trust.configured()) {
    announcement.capabilities.push_back("radio.wifi.scan");
    announcement.capabilities.push_back("tool.dns.lookup");
    announcement.capabilities.push_back("net.tcp.connect");
    announcement.capabilities.push_back("net.service.identify");
    announcement.capabilities.push_back("net.discovery.scan");
    announcement.capabilities.push_back("net.hosts.snapshot");
    announcement.capabilities.push_back("assessment.findings.snapshot");
    announcement.capabilities.push_back("storage.evidence.manifest");
    announcement.capabilities.push_back("coordination.job.status");
    announcement.capabilities.push_back("coordination.job.cancel");
  }
  announcement.status = "ready";

  reconclave::CapabilityDescriptor system_info_descriptor;
  system_info_descriptor.id = "system.info";
  system_info_descriptor.permission = "public";

  reconclave::CapabilityDescriptor wifi_scan_descriptor;
  wifi_scan_descriptor.id = "radio.wifi.scan";
  wifi_scan_descriptor.permission = "trusted";
  reconclave::CapabilityDescriptor arp_descriptor;
  arp_descriptor.id = "net.arp.snapshot";
  arp_descriptor.permission = "public";
  arp_descriptor.features = {"passive", "ipv4", "mac-address"};
  reconclave::CapabilityDescriptor ssh_descriptor;
  ssh_descriptor.id = "system.ssh.status";
  ssh_descriptor.permission = "public";
  ssh_descriptor.features = {"openssh-client", "openssh-server", "key-only", "sftp", "scp"};
  announcement.capability_descriptors = {system_info_descriptor, ssh_descriptor, arp_descriptor};
  if (trust.configured()) {
    announcement.capability_descriptors.push_back(wifi_scan_descriptor);
    reconclave::CapabilityDescriptor dns_descriptor;
    dns_descriptor.id = "tool.dns.lookup";
    dns_descriptor.permission = "trusted";
    dns_descriptor.features = {"ipv4", "ipv6", "bounded-results"};
    reconclave::CapabilityDescriptor tcp_descriptor;
    tcp_descriptor.id = "net.tcp.connect";
    tcp_descriptor.permission = "trusted";
    tcp_descriptor.features = {"ipv4", "scope-required", "bounded-timeout"};
    announcement.capability_descriptors.push_back(dns_descriptor);
    announcement.capability_descriptors.push_back(tcp_descriptor);
    reconclave::CapabilityDescriptor service_descriptor;
    service_descriptor.id = "net.service.identify";
    service_descriptor.permission = "trusted";
    service_descriptor.features = {"ipv4", "scope-required", "bounded-banner",
                                   "no-authentication", "fixed-http-head"};
    announcement.capability_descriptors.push_back(service_descriptor);
    reconclave::CapabilityDescriptor discovery_descriptor;
    discovery_descriptor.id = "net.discovery.scan";
    discovery_descriptor.permission = "trusted";
    discovery_descriptor.features = {"ipv4", "range", "scope-required", "cancellable", "bounded-ipv4-24"};
    discovery_descriptor.weight = 4;
    reconclave::CapabilityDescriptor status_descriptor;
    status_descriptor.id = "coordination.job.status";
    status_descriptor.permission = "trusted";
    status_descriptor.features = {"job-id-addressed"};
    reconclave::CapabilityDescriptor cancel_descriptor;
    cancel_descriptor.id = "coordination.job.cancel";
    cancel_descriptor.permission = "trusted";
    cancel_descriptor.features = {"job-id-addressed", "idempotent"};
    announcement.capability_descriptors.push_back(discovery_descriptor);
    reconclave::CapabilityDescriptor hosts_descriptor;
    hosts_descriptor.id = "net.hosts.snapshot";
    hosts_descriptor.permission = "trusted";
    hosts_descriptor.features = {"persistent", "first-last-seen", "change-count"};
    announcement.capability_descriptors.push_back(hosts_descriptor);
    reconclave::CapabilityDescriptor findings_descriptor;
    findings_descriptor.id = "assessment.findings.snapshot";
    findings_descriptor.permission = "trusted";
    findings_descriptor.features = {"evidence-derived", "deduplicated", "non-exploitative"};
    announcement.capability_descriptors.push_back(findings_descriptor);
    reconclave::CapabilityDescriptor manifest_descriptor;
    manifest_descriptor.id = "storage.evidence.manifest";
    manifest_descriptor.permission = "trusted";
    manifest_descriptor.features = {"sha256", "timeline-integrity", "bounded-files"};
    announcement.capability_descriptors.push_back(manifest_descriptor);
    announcement.capability_descriptors.push_back(status_descriptor);
    announcement.capability_descriptors.push_back(cancel_descriptor);
  }

  const auto validation = reconclave::validate(announcement);
  if (!validation.errors.empty()) {
    for (const auto& error : validation.errors) std::fprintf(stderr, "invalid announcement: %s\n", error.c_str());
    return 1;
  }

  // The init script starts this after S40network brings the interface up,
  // but DHCP itself (udhcpc) runs asynchronously in the background - an IP
  // address is not guaranteed yet at that point. Confirmed on real hardware:
  // a cold boot reached this line before DHCP completed and exited
  // immediately. Retry rather than fail once, matching how
  // /etc/init.d/S99zz_k230_phone_ui itself waits (up to 10s) for
  // /dev/dri/card0 to exist before giving up.
  std::string local_ip;
  for (int attempt = 0; attempt < 20 && local_ip.empty(); ++attempt) {
    local_ip = findLocalIpv4();
    if (local_ip.empty()) sleep(1);
  }
  if (local_ip.empty()) {
    std::fprintf(stderr, "no non-loopback IPv4 address found after waiting\n");
    return 1;
  }
  std::printf("reconclave k230: device_id=%s ip=%s http_port=%u\n", kDeviceId, local_ip.c_str(), kHttpPort);

  reconclave::HttpServer http;
  if (!http.listen(kHttpPort)) {
    std::fprintf(stderr, "failed to listen on port %u\n", kHttpPort);
    return 1;
  }

  reconclave::MdnsResponder mdns;
  if (!mdns.start(kDeviceId, kServiceType, kHttpPort, local_ip, "device_id", kDeviceId)) {
    std::fprintf(stderr, "failed to start mDNS responder\n");
    return 1;
  }

  const double started_at = monotonicSeconds();
  std::uint64_t sequence = 0;
  std::string last_evidenced_job_id;

  auto appendEvidence = [&](const std::string& kind, reconclave::json::Value result) {
    std::string record_hash;
    std::string evidence_error;
    if (evidence.append(kind, result, reconclave::loadEvidenceContext(kSessionPath),
                        record_hash, evidence_error)) {
      result.set("evidence_record_hash", reconclave::json::Value::makeString(record_hash));
    } else {
      result.set("evidence_error", reconclave::json::Value::makeString(evidence_error));
    }
    return result;
  };

  auto httpHandler = [&](const reconclave::HttpRequest& request) -> reconclave::HttpResponse {
    reconclave::HttpResponse response;
    if (request.method == "GET" && request.path == kAnnouncePath) {
      auto envelope = makeEnvelope("announce", "", ++sequence);
      envelope.set("payload", announcementToJson(announcement, trust));
      response.body = reconclave::json::stringify(envelope);
      return response;
    }
    if (request.method == "POST" && request.path == kMessagePath) {
      reconclave::json::Value request_json;
      std::string parse_error;
      if (!reconclave::json::parse(request.body, request_json, parse_error)) {
        response.body = R"({"error":"invalid_json"})";
        response.status = 400;
        return response;
      }
      const auto* proto = request_json.find("proto");
      const auto* type = request_json.find("type");
      const auto* destination = request_json.find("destination_node");
      const auto* source = request_json.find("source_node");
      const auto* payload = request_json.find("payload");
      const std::string source_id = source ? source->asString() : "unknown";

      auto envelope = makeEnvelope("response", source_id, ++sequence);
      std::string request_id = "invalid-request";
      if (payload != nullptr) {
        if (const auto* request_id_field = payload->find("request_id")) {
          request_id = request_id_field->asString("invalid-request");
        }
      }

      const bool valid = proto && proto->asString() == reconclave::kProtocolVersion && type &&
                          type->asString() == "request" && destination &&
                          destination->asString() == kDeviceId;

      reconclave::json::Value response_payload = reconclave::json::Value::makeObject();
      response_payload.set("request_id", reconclave::json::Value::makeString(request_id));
      if (!valid) {
        response_payload.set("status", reconclave::json::Value::makeString("rejected"));
        reconclave::json::Value error = reconclave::json::Value::makeObject();
        error.set("code", reconclave::json::Value::makeString("INVALID_REQUEST"));
        error.set("message", reconclave::json::Value::makeString("Malformed or misdirected request"));
        response_payload.set("error", error);
      } else {
        std::string capability;
        if (const auto* capability_field = payload ? payload->find("capability") : nullptr) {
          capability = capability_field->asString();
        }
        const auto* arguments = payload ? payload->find("arguments") : nullptr;
        const auto* auth = payload ? payload->find("auth") : nullptr;
        const bool trusted_capability = capability == "radio.wifi.scan" || capability == "tool.dns.lookup" || capability == "net.tcp.connect" ||
            capability == "net.service.identify" ||
            capability == "net.discovery.scan" || capability == "net.hosts.snapshot" ||
            capability == "assessment.findings.snapshot" ||
            capability == "storage.evidence.manifest" ||
            capability == "coordination.job.status" ||
            capability == "coordination.job.cancel";
        reconclave::TrustDecision trust_decision{true, "", "", ""};
        std::string authenticated_nonce;
        if (trusted_capability) {
          if (arguments == nullptr || auth == nullptr) {
            trust_decision = {false, "UNAUTHENTICATED", "request is not signed", ""};
          } else {
            trust_decision = trust.verifyRequest(source_id, request_id, capability, *arguments, *auth);
            authenticated_nonce = trust_decision.nonce;
            if (trust_decision.allowed && (capability == "net.tcp.connect" ||
                capability == "net.service.identify" || capability == "net.discovery.scan")) {
              trust_decision = trust.verifyDelegatedScope(capability, *arguments, nowMillis());
            }
          }
        }
        if (!trust_decision.allowed) {
          response_payload.set("status", reconclave::json::Value::makeString("rejected"));
          reconclave::json::Value error = reconclave::json::Value::makeObject();
          error.set("code", reconclave::json::Value::makeString(trust_decision.code));
          error.set("message", reconclave::json::Value::makeString(trust_decision.message));
          response_payload.set("error", error);
        } else if (capability != "system.info" && capability != "system.ssh.status" &&
                   capability != "net.arp.snapshot" && !trusted_capability) {
          response_payload.set("status", reconclave::json::Value::makeString("rejected"));
          reconclave::json::Value error = reconclave::json::Value::makeObject();
          error.set("code", reconclave::json::Value::makeString("CAPABILITY_UNAVAILABLE"));
          error.set("message", reconclave::json::Value::makeString("Capability is not available"));
          response_payload.set("error", error);
        } else {
          response_payload.set("status", reconclave::json::Value::makeString("ok"));
          if (capability == "system.info") response_payload.set("result", systemInfo(started_at));
          else if (capability == "system.ssh.status") response_payload.set("result", sshInfo());
          else if (capability == "net.arp.snapshot") response_payload.set("result", arpSnapshot());
          else if (capability == "radio.wifi.scan") {
            response_payload.set("result", appendEvidence("wifi.survey", wifiSurvey()));
          } else if (capability == "tool.dns.lookup") {
            response_payload.set("result", appendEvidence("dns.lookup", dnsLookup(*arguments)));
          } else if (capability == "net.tcp.connect") {
            response_payload.set("result", appendEvidence("tcp.observation", tcpCheck(*arguments)));
          } else if (capability == "net.service.identify") {
            reconclave::ServiceObservation observation;
            auto result = serviceIdentify(*arguments, &observation);
            const unsigned changes = host_knowledge.observeService(observation, nowMillis());
            if (observation.connected && !host_knowledge.save(host_error)) {
              result.set("knowledge_error", reconclave::json::Value::makeString(host_error));
            }
            result.set("host_changes", reconclave::json::Value::makeNumber(changes));
            response_payload.set("result", appendEvidence("service.observation", result));
          } else if (capability == "net.hosts.snapshot") {
            response_payload.set("result", host_knowledge.toJson());
          } else if (capability == "assessment.findings.snapshot") {
            response_payload.set("result", host_knowledge.findingsJson());
          } else if (capability == "storage.evidence.manifest") {
            reconclave::json::Value manifest;
            std::string manifest_error;
            if (evidence.createManifest(reconclave::loadEvidenceContext(kSessionPath), kFirmware,
                                        manifest, manifest_error)) {
              response_payload.set("result", manifest);
            } else {
              response_payload.set("status", reconclave::json::Value::makeString("rejected"));
              reconclave::json::Value error = reconclave::json::Value::makeObject();
              error.set("code", reconclave::json::Value::makeString("EVIDENCE_INTEGRITY_ERROR"));
              error.set("message", reconclave::json::Value::makeString(manifest_error));
              response_payload.set("error", error);
            }
          }
          else if (capability == "net.discovery.scan") {
            std::string error;
            const auto* network = arguments->find("network");
            if (network == nullptr || !discovery.start(network->asString(), error)) {
              reconclave::json::Value result = reconclave::json::Value::makeObject();
              result.set("started", reconclave::json::Value::makeBool(false));
              result.set("error", reconclave::json::Value::makeString(error.empty() ? "network is required" : error));
              response_payload.set("result", result);
            } else {
              response_payload.set("result", appendEvidence("discovery.started",
                                                              discoverySnapshot(discovery.snapshot())));
            }
          } else {
            const auto current = discovery.snapshot();
            const auto* job_id = arguments->find("job_id");
            if (!job_id || job_id->asString().empty() || job_id->asString() != current.job_id) {
              response_payload.set("status", reconclave::json::Value::makeString("rejected"));
              reconclave::json::Value error = reconclave::json::Value::makeObject();
              error.set("code", reconclave::json::Value::makeString("JOB_NOT_FOUND"));
              error.set("message", reconclave::json::Value::makeString(
                  "No discovery job matches the supplied job_id"));
              response_payload.set("error", error);
            } else {
              if (capability == "coordination.job.cancel") discovery.cancel();
              response_payload.set("result", discoverySnapshot(discovery.snapshot()));
            }
          }
        }
        if (trusted_capability && !authenticated_nonce.empty()) {
          const auto* status = response_payload.find("status");
          const auto* body = response_payload.find("result");
          if (body == nullptr) body = response_payload.find("error");
          if (status != nullptr && body != nullptr) {
            response_payload.set("auth", trust.responseAuth(source_id, request_id,
                                 status->asString(), *body, authenticated_nonce));
          }
        }
      }
      envelope.set("payload", response_payload);
      response.body = reconclave::json::stringify(envelope);
      return response;
    }
    response.status = 404;
    response.body = R"({"error":"not_found"})";
    return response;
  };

  std::printf("reconclave k230: serving %s and %s, mDNS on %s._reconclave._tcp.local.\n", kAnnouncePath,
              kMessagePath, kDeviceId);
  writeRuntimeStatus(trust, announcement, discovery.snapshot(), local_ip,
                     host_knowledge.snapshot().size());
  std::uint64_t next_status_write = nowMillis() + 1000;
  std::uint64_t next_passive_inventory = nowMillis();

  while (g_running) {
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(http.socketFd(), &read_fds);
    FD_SET(mdns.socketFd(), &read_fds);
    int max_fd = std::max(http.socketFd(), mdns.socketFd());

    struct timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = 200000;

    int ready = select(max_fd + 1, &read_fds, nullptr, nullptr, &timeout);
    if (ready < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (ready > 0) {
      if (FD_ISSET(http.socketFd(), &read_fds)) http.acceptAndHandle(httpHandler);
      if (FD_ISSET(mdns.socketFd(), &read_fds)) mdns.handleQuery();
    }
    struct stat command_info {};
    if (lstat(kLocalCommandPath, &command_info) == 0 && S_ISREG(command_info.st_mode) &&
        command_info.st_uid == geteuid() && (command_info.st_mode & 0022) == 0) {
      std::ifstream command_file(kLocalCommandPath);
      std::string command;
      std::getline(command_file, command);
      command_file.close();
      unlink(kLocalCommandPath);
      if (command.rfind("DISCOVERY ", 0) == 0) {
        std::string start_error;
        if (discovery.start(command.substr(10), start_error)) {
          appendEvidence("discovery.local.started", discoverySnapshot(discovery.snapshot()));
        }
      } else if (command == "CANCEL") {
        discovery.cancel();
      } else if (command.rfind("IDENTIFY ", 0) == 0) {
        // Operator-triggered re-probe of one host's already-known open ports,
        // fired from the touch UI's Host Detail screen via the same
        // local-command file as DISCOVERY. No trust/scope check here for the
        // same reason DISCOVERY has none: this file is only writable by this
        // same device's own euid (checked above), so it can only ever
        // originate from something already running as root on this board -
        // the touch UI, not a remote coordinator. Format: "IDENTIFY
        // <address> <port>[,<port>...]".
        std::istringstream rest(command.substr(9));
        std::string address, port_list;
        rest >> address >> port_list;
        std::istringstream ports(port_list);
        std::string port_text;
        while (std::getline(ports, port_text, ',')) {
          const long port = std::strtol(port_text.c_str(), nullptr, 10);
          if (port <= 0 || port > 65535 || address.empty()) continue;
          reconclave::ServiceObservation observation;
          reconclave::identifyService(address, static_cast<std::uint16_t>(port), 800, observation);
          const unsigned changes = host_knowledge.observeService(observation, nowMillis());
          if (observation.connected) {
            std::string save_error;
            host_knowledge.save(save_error);
            reconclave::json::Value evidence_body = reconclave::json::Value::makeObject();
            evidence_body.set("address", reconclave::json::Value::makeString(address));
            evidence_body.set("port", reconclave::json::Value::makeNumber(port));
            evidence_body.set("service", reconclave::json::Value::makeString(observation.service));
            evidence_body.set("banner", reconclave::json::Value::makeString(observation.banner));
            evidence_body.set("host_changes", reconclave::json::Value::makeNumber(changes));
            appendEvidence("service.observation.local", evidence_body);
          }
        }
      }
    }
    if (nowMillis() >= next_status_write) {
      const auto job = discovery.snapshot();
      if (!job.job_id.empty() && job.status != "running" &&
          job.job_id != last_evidenced_job_id) {
        const unsigned changes = host_knowledge.observeDiscovery(job.hosts, nowMillis());
        host_knowledge.save(host_error);
        auto completed = discoverySnapshot(job);
        completed.set("host_changes", reconclave::json::Value::makeNumber(changes));
        appendEvidence("discovery." + job.status, completed);
        last_evidenced_job_id = job.job_id;
      }
      if (nowMillis() >= next_passive_inventory) {
        std::vector<reconclave::ArpNeighbour> neighbours;
        std::string arp_error;
        if (reconclave::readArpSnapshot(neighbours, arp_error)) {
          const unsigned changes = host_knowledge.observeArp(neighbours, nowMillis());
          if (host_knowledge.save(host_error) && changes > 0) {
            auto change = reconclave::json::Value::makeObject();
            change.set("changes", reconclave::json::Value::makeNumber(changes));
            change.set("known_hosts", reconclave::json::Value::makeNumber(
                static_cast<double>(host_knowledge.snapshot().size())));
            appendEvidence("hosts.changed", change);
          }
        }
        next_passive_inventory = nowMillis() + 30000;
      }
      writeRuntimeStatus(trust, announcement, job, local_ip, host_knowledge.snapshot().size());
      next_status_write = nowMillis() + 1000;
    }
  }

  std::printf("reconclave k230: shutting down\n");
  mdns.goodbye();
  return 0;
}
