#include "reconclave/node_registry.h"
#include "reconclave/protocol.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void expect(bool condition, const std::string& description) {
  if (!condition) {
    std::cerr << "FAIL: " << description << '\n';
    ++failures;
  }
}

reconclave::NodeAnnouncement p4Announcement() {
  return {"rc-p4-01", "poe-p4", "0.1.0", {"node"},
          {"system.info", "net.discovery.scan", "net.tcp.connect"}, "ready"};
}

}  // namespace

int main() {
  using namespace reconclave;

  expect(messageTypeFromString("announce") == MessageType::Announce,
         "announcement type parses");
  expect(messageTypeFromString("ANNOUNCE") == MessageType::Unknown,
         "message types are case-sensitive");
  expect(isValidCapability("net.discovery.scan"), "namespaced capability is valid");
  expect(!isValidCapability("scan"), "unnamespaced capability is rejected");
  expect(!isValidCapability("net..scan"), "empty capability segment is rejected");

  Envelope envelope{kProtocolVersion, MessageType::Announce, "msg-1", "rc-p4-01", "",
                    1787688000000ULL, 1, "{}"};
  expect(validate(envelope).valid, "valid protocol envelope is accepted");
  expect(validate(envelope, p4Announcement()).valid,
         "announcement envelope and payload validate together");
  auto mismatched = p4Announcement();
  mismatched.device_id = "rc-p4-impostor";
  expect(!validate(envelope, mismatched).valid,
         "announcement identity must match envelope source");
  envelope.proto = "reconclave/99";
  expect(!validate(envelope).valid, "unsupported protocol version is rejected");

  auto p4 = p4Announcement();
  expect(validate(p4).valid, "valid node announcement is accepted");
  p4.capabilities.push_back("net.discovery.scan");
  expect(!validate(p4).valid, "duplicate capabilities are rejected");

  CapabilityResponse accepted{"req-1", ResponseStatus::Accepted, "job-1", "{}", "", ""};
  expect(validate(accepted).valid, "accepted asynchronous response has a job id");
  accepted.job_id.clear();
  expect(!validate(accepted).valid, "accepted response without job id is rejected");

  StreamChunk chunk{"evidence-1", 0, false, "base64", "SGVsbG8="};
  expect(validate(chunk).valid, "base64 stream chunk is valid");
  chunk.encoding = "raw";
  expect(!validate(chunk).valid, "unknown stream encoding is rejected");

  NodeRegistry registry;
  expect(registry.observe(p4Announcement(), 1000), "registry accepts valid announcement");
  expect(registry.size() == 1, "registry contains one node");
  expect(registry.providersOf("net.tcp.connect").size() == 1,
         "registry resolves a provider by capability");

  auto updated = p4Announcement();
  updated.status = "busy";
  expect(registry.observe(updated, 2000), "registry updates an existing node");
  expect(registry.size() == 1, "update does not duplicate node");
  expect(registry.find("rc-p4-01") != nullptr &&
             registry.find("rc-p4-01")->announcement.status == "busy",
         "updated node state is visible");
  expect(!registry.observe(updated, 1999), "out-of-order announcement is rejected");
  updated.device_type = "cardputer-adv";
  expect(!registry.observe(updated, 2001), "device type identity collision is rejected");
  expect(registry.expireBefore(2001) == 1, "stale node expires");
  expect(registry.providersOf("net.tcp.connect").empty(),
         "lost node capability becomes unavailable");

  if (failures == 0) std::cout << "All Reconclave protocol tests passed\n";
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
