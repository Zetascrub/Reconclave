#include <cstdio>
#include <fstream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

#include "network_inventory.h"
#include "wifi_scanner.h"
#include "gnss_receiver.h"
#include "network_tools.h"
#include "trust_crypto.h"
#include "trust_policy.h"
#include "json.h"
#include "discovery_job.h"
#include "evidence_store.h"
#include "host_knowledge.h"

namespace {

int failures = 0;

void expect(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
  }
}

}  // namespace

int main() {
  const std::string fixture = "/tmp/reconclave-k230-arp-test.txt";
  {
    std::ofstream out(fixture);
    out << "IP address HW type Flags HW address Mask Device\n"
        << "192.0.2.1 0x1 0x2 00:11:22:33:44:55 * eth0\n"
        << "192.0.2.9 0x1 0x0 00:00:00:00:00:00 * eth0\n";
  }
  std::vector<reconclave::ArpNeighbour> neighbours;
  std::string error;
  expect(reconclave::readArpSnapshot(neighbours, error, fixture), "ARP fixture parses");
  expect(neighbours.size() == 2, "ARP snapshot preserves entries");
  expect(neighbours[0].complete, "complete ARP entry identified");
  expect(!neighbours[1].complete, "incomplete ARP entry identified");
  std::remove(fixture.c_str());

  std::vector<reconclave::WifiObservation> wifi(3);
  wifi[0].ssid = "office"; wifi[0].channel = 1; wifi[0].rssi_dbm = -40; wifi[0].secured = true;
  wifi[1].channel = 6; wifi[1].rssi_dbm = -75; wifi[1].secured = false;
  wifi[2].ssid = "lab-5g"; wifi[2].channel = 36; wifi[2].rssi_dbm = -55; wifi[2].secured = true;
  const auto summary = reconclave::analyseWifi(wifi);
  expect(summary.total == 3, "Wi-Fi total computed");
  expect(summary.open == 1 && summary.hidden == 1, "Wi-Fi risk counts computed");
  expect(summary.two_ghz == 2 && summary.five_ghz == 1, "Wi-Fi bands classified");
  expect(summary.strongest_rssi_dbm == -40, "strongest AP computed");
  expect(summary.best_24_channel == 11, "least-congested 2.4 GHz channel computed");

  reconclave::GnssFix fix;
  expect(reconclave::parseNmeaSentence(
      "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47", fix),
      "checksum-valid GGA parses");
  expect(fix.valid && fix.satellites == 8, "GGA fix and satellite count parsed");
  expect(fix.latitude > 48.1172 && fix.latitude < 48.1174, "latitude converted to decimal degrees");
  expect(fix.longitude > 11.5166 && fix.longitude < 11.5168, "longitude converted to decimal degrees");
  expect(!reconclave::parseNmeaSentence(
      "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*00", fix),
      "bad NMEA checksum rejected");

  expect(reconclave::validHostname("localhost"), "simple hostname accepted");
  expect(reconclave::validHostname("scanner.lab.example"), "dotted hostname accepted");
  expect(!reconclave::validHostname("bad host; reboot"), "shell-like hostname rejected");
  reconclave::DnsResult dns;
  expect(reconclave::resolveHost("localhost", dns, error) && !dns.addresses.empty(),
         "bounded DNS lookup resolves localhost");
  reconclave::TcpObservation tcp;
  expect(!reconclave::tcpConnect("example.invalid", 443, 500, tcp) &&
             tcp.error == "numeric IPv4 address required",
         "TCP tool refuses hostname/scope ambiguity");
  expect(!reconclave::tcpConnect("127.0.0.1", 0, 500, tcp), "TCP tool rejects port zero");
  expect(reconclave::classifyService(22, "SSH-2.0-OpenSSH") == "ssh",
         "service classifier recognizes SSH banner");
  expect(reconclave::classifyService(8080, "") == "http",
         "service classifier uses conservative well-known port fallback");
  expect(reconclave::classifyService(65000, "") == "unknown",
         "service classifier does not invent an unknown service");

  expect(reconclave::sha256Hex("abc") ==
             "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
         "SHA-256 matches published vector");
  reconclave::TrustKey key{};
  key.fill(0x0b);
  expect(reconclave::hmacSha256Hex(key, "Hi There") ==
             "198a607eb44bfbc69903a0f1cf2bbdc5ba0aa3f3d9ae3c1c7a3b1696a0b68cf7",
         "HMAC-SHA256 matches 32-byte-key vector");
  reconclave::json::Value unordered = reconclave::json::Value::makeObject();
  unordered.set("z", reconclave::json::Value::makeNumber(1));
  unordered.set("tag", reconclave::json::Value::makeString("ignored"));
  unordered.set("a", reconclave::json::Value::makeBool(true));
  expect(reconclave::json::canonicalStringify(unordered, "tag") == "{\"a\":true,\"z\":1}",
         "canonical JSON sorts keys and omits signature tag");

  reconclave::TrustPolicy policy("rc-k230-test");
  policy.setExecutionKeyForTest(key);
  policy.setBootNonceForTest("0011223344556677");
  auto arguments = reconclave::json::Value::makeObject();
  arguments.set("target", reconclave::json::Value::makeString("192.0.2.20"));
  const std::string argument_digest = reconclave::sha256Hex(reconclave::json::canonicalStringify(arguments));
  auto auth = reconclave::json::Value::makeObject();
  auth.set("nonce", reconclave::json::Value::makeString("0123456789abcdef"));
  auth.set("payload_digest", reconclave::json::Value::makeString(argument_digest));
  auth.set("coordinator_priority", reconclave::json::Value::makeNumber(10));
  auth.set("lease_ms", reconclave::json::Value::makeNumber(30000));
  const std::string request_canonical = "coordinator|rc-k230-test|request-1|net.tcp.connect|"
      "0011223344556677|" + argument_digest + "|0123456789abcdef|10|30000";
  auth.set("tag", reconclave::json::Value::makeString(
      reconclave::hmacSha256Hex(key, request_canonical, 16)));
  expect(policy.verifyRequest("coordinator", "request-1", "net.t.connect", arguments, auth).allowed == false,
         "request signature is capability-bound");
  auto accepted = policy.verifyRequest("coordinator", "request-1", "net.tcp.connect", arguments, auth);
  expect(accepted.allowed, "valid authenticated request accepted");
  expect(!policy.verifyRequest("coordinator", "request-1", "net.tcp.connect", arguments, auth).allowed,
         "authenticated request nonce cannot be replayed");
  auto non_hex_auth = auth;
  non_hex_auth.set("nonce", reconclave::json::Value::makeString("not-a-hex-nonce!!"));
  expect(!policy.verifyRequest("coordinator", "request-2", "net.tcp.connect", arguments,
                               non_hex_auth).allowed,
         "non-hex request nonce is rejected before signature verification");
  auto response_body = reconclave::json::Value::makeObject();
  response_body.set("connected", reconclave::json::Value::makeBool(true));
  const auto response_auth = policy.responseAuth("coordinator", "request-1", "ok", response_body,
                                                  "0123456789abcdef");
  const std::string response_digest = reconclave::sha256Hex(
      reconclave::json::canonicalStringify(response_body));
  const std::string response_canonical = "rc-k230-test|coordinator|request-1|ok|0011223344556677|" +
      response_digest + "|0123456789abcdef";
  expect(response_auth.find("tag") != nullptr &&
             response_auth.find("tag")->asString() == reconclave::hmacSha256Hex(key, response_canonical, 16),
         "response authentication binds result and request nonce");

  auto scope_arguments = reconclave::json::Value::makeObject();
  scope_arguments.set("target", reconclave::json::Value::makeString("192.0.2.20"));
  auto token = reconclave::json::Value::makeObject();
  token.set("capability", reconclave::json::Value::makeString("net.tcp.connect"));
  token.set("destination_node", reconclave::json::Value::makeString("rc-k230-test"));
  token.set("arguments_digest", reconclave::json::Value::makeString(
      reconclave::sha256Hex(reconclave::json::canonicalStringify(scope_arguments))));
  token.set("issued_at_ms", reconclave::json::Value::makeNumber(100000));
  token.set("expires_at_ms", reconclave::json::Value::makeNumber(200000));
  auto includes = reconclave::json::Value::makeArray();
  includes.push_back(reconclave::json::Value::makeString("192.0.2.0/24"));
  token.set("included_networks", includes);
  auto excludes = reconclave::json::Value::makeArray();
  excludes.push_back(reconclave::json::Value::makeString("192.0.2.99/32"));
  token.set("excluded_networks", excludes);
  token.set("tag", reconclave::json::Value::makeString(
      reconclave::hmacSha256Hex(key, reconclave::json::canonicalStringify(token, "tag"))));
  scope_arguments.set("_scope_delegation", token);
  expect(policy.verifyDelegatedScope("net.tcp.connect", scope_arguments, 150000).allowed,
         "valid in-range delegated scope accepted");
  expect(!policy.verifyDelegatedScope("net.tcp.connect", scope_arguments, 250000).allowed,
         "expired delegated scope rejected");
  auto excluded_arguments = scope_arguments;
  excluded_arguments.set("target", reconclave::json::Value::makeString("192.0.2.99"));
  expect(!policy.verifyDelegatedScope("net.tcp.connect", excluded_arguments, 150000).allowed,
         "excluded delegated-scope target rejected");
  auto outside_arguments = scope_arguments;
  outside_arguments.set("target", reconclave::json::Value::makeString("198.51.100.4"));
  expect(!policy.verifyDelegatedScope("net.tcp.connect", outside_arguments, 150000).allowed,
         "target modification invalidates argument binding");
  auto tampered_arguments = scope_arguments;
  auto tampered_token = token;
  tampered_token.set("tag", reconclave::json::Value::makeString(std::string(64, '0')));
  tampered_arguments.set("_scope_delegation", tampered_token);
  expect(!policy.verifyDelegatedScope("net.tcp.connect", tampered_arguments, 150000).allowed,
         "tampered delegated-scope signature rejected");

  std::uint32_t first = 0, last = 0;
  expect(reconclave::boundedIpv4Range("192.0.2.0/24", first, last, error) && last - first + 1 == 254,
         "discovery accepts and bounds a /24");
  expect(!reconclave::boundedIpv4Range("192.0.0.0/23", first, last, error),
         "discovery rejects networks larger than /24");
  reconclave::DiscoveryJob discovery;
  expect(reconclave::boundedIpv4Range("127.0.0.1/32", first, last, error) && first == last,
         "single-host discovery range accepted");
  expect(discovery.start("127.0.0.1/32", error), "discovery job starts");
  for (int i = 0; i < 100 && discovery.snapshot().status == "running"; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  const auto discovery_result = discovery.snapshot();
  expect(discovery_result.status == "complete" && discovery_result.checked == 1,
         "discovery job completes with bounded progress");

  const std::string evidence_directory =
      "/tmp/reconclave-evidence-test-" + std::to_string(static_cast<long long>(getpid()));
  expect(mkdir(evidence_directory.c_str(), 0700) == 0, "evidence fixture directory created");
  reconclave::EvidenceStore evidence(evidence_directory, "rc-k230-test");
  reconclave::EvidenceContext context{"PROJECT-1", "ENGAGEMENT-1", "operator"};
  auto evidence_data = reconclave::json::Value::makeObject();
  evidence_data.set("target", reconclave::json::Value::makeString("192.0.2.20"));
  std::string first_hash;
  expect(evidence.append("tcp.observation", evidence_data, context, first_hash, error) &&
             first_hash.size() == 64,
         "first evidence record appended and hashed");
  evidence_data.set("port", reconclave::json::Value::makeNumber(443));
  std::string second_hash;
  expect(evidence.append("tcp.observation", evidence_data, context, second_hash, error) &&
             second_hash != first_hash,
         "second evidence record is uniquely chained");
  const auto intact_evidence = evidence.verify();
  expect(intact_evidence.valid && intact_evidence.records == 2,
         "intact evidence timeline verifies");
  const std::string attachment_path = evidence_directory + "/notes.txt";
  {
    std::ofstream attachment(attachment_path);
    attachment << "authorised assessment note\n";
  }
  reconclave::json::Value manifest;
  expect(evidence.createManifest(context, "test-firmware", manifest, error),
         "verified evidence manifest is created");
  expect(manifest.find("manifest_hash") != nullptr &&
             manifest.find("manifest_hash")->asString().size() == 64 &&
             manifest.find("timeline_records") != nullptr &&
             manifest.find("timeline_records")->asUInt64() == 2,
         "manifest binds timeline state and carries its own hash");
  {
    std::fstream timeline(evidence.timelinePath(), std::ios::in | std::ios::out);
    char first_character = '\0';
    timeline.get(first_character);
    timeline.seekp(0);
    timeline.put(first_character == '{' ? '[' : '{');
  }
  const auto corrupt_evidence = evidence.verify();
  expect(!corrupt_evidence.valid && corrupt_evidence.first_invalid_record == 1,
         "tampered evidence timeline is detected");
  std::remove(evidence.timelinePath().c_str());
  std::remove(attachment_path.c_str());
  std::remove((evidence_directory + "/manifest.json").c_str());
  rmdir(evidence_directory.c_str());

  const std::string hosts_path =
      "/tmp/reconclave-hosts-test-" + std::to_string(static_cast<long long>(getpid())) + ".json";
  reconclave::HostKnowledge knowledge(hosts_path);
  std::vector<reconclave::ArpNeighbour> arp_hosts = {
      {"192.0.2.20", "00:11:22:33:44:55", "eth0", true},
      {"192.0.2.21", "", "eth0", false}};
  expect(knowledge.observeArp(arp_hosts, 1000) == 0 && knowledge.snapshot().size() == 1,
         "host knowledge stores only complete ARP observations");
  std::vector<reconclave::DiscoveredHost> discovered = {{"192.0.2.20", {443, 22, 443}}};
  expect(knowledge.observeDiscovery(discovered, 2000) == 0,
         "first service observation establishes a baseline");
  discovered[0].open_ports = {22, 80};
  expect(knowledge.observeDiscovery(discovered, 3000) == 1,
         "service-set change is detected");
  reconclave::ServiceObservation identified;
  identified.connected = true;
  identified.address = "192.0.2.20";
  identified.port = 22;
  identified.service = "ssh";
  identified.banner = "SSH-2.0-Test";
  expect(knowledge.observeService(identified, 4000) == 0,
         "first service identity establishes a baseline");
  identified.banner = "SSH-2.0-Test-New";
  expect(knowledge.observeService(identified, 5000) == 1,
         "service banner change is detected");
  expect(knowledge.save(error), "host knowledge saves atomically");
  reconclave::HostKnowledge reloaded(hosts_path);
  expect(reloaded.load(error), "host knowledge reloads");
  const auto host_snapshot = reloaded.snapshot();
  expect(host_snapshot.size() == 1 && host_snapshot[0].observations == 5 &&
             host_snapshot[0].changes == 2 && host_snapshot[0].open_ports.size() == 2 &&
             host_snapshot[0].services.size() == 1 &&
             host_snapshot[0].services[0].banner == "SSH-2.0-Test-New",
         "host history survives persistence");
  identified.port = 23;
  identified.service = "telnet";
  identified.banner.clear();
  identified.attention = "cleartext remote administration";
  expect(reloaded.observeService(identified, 6000) == 0 && reloaded.attentionCount() == 1,
         "attention-worthy service is retained without overstating severity");
  const auto findings = reloaded.findingsJson();
  expect(findings.find("count") != nullptr && findings.find("count")->asUInt64() == 2,
         "derived findings include service review and asset-change observations");
  std::remove(hosts_path.c_str());
  return failures == 0 ? 0 : 1;
}
