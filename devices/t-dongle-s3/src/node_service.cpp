#include "node_service.h"

#include <WiFi.h>

#include "reconclave/protocol.h"  // shared domain library (proves fleet integration)
#include "framework.h"            // Config
#include "status_led.h"

namespace reconclave {
namespace {
constexpr const char* kFirmware = "0.1.0-node.1";
constexpr const char* kDeviceType = "t-dongle-s3";
constexpr size_t kMaxBodyBytes = reconclave::kMaxPayloadBytes;  // 16 KiB
}  // namespace

void NodeService::beginIdentity(Config& config) {
  (void)config;
  String mac = WiFi.macAddress();  // "AA:BB:CC:DD:EE:FF"
  mac.replace(":", "");
  mac.toLowerCase();
  device_id_ = "rc-tdongle-" + mac;
  trust_.begin();
  Serial.printf("node: id=%s boot_nonce=%s\n", device_id_.c_str(), trust_.bootNonceHex());
}

void NodeService::registerCapability(const String& id, const String& permission,
                                     CapabilityHandler handler) {
  capabilities_.push_back({id, permission, std::move(handler)});
  Serial.printf("node: capability registered: %s (%s)\n", id.c_str(), permission.c_str());
}

const NodeService::Registered* NodeService::find(const String& id) const {
  for (const auto& c : capabilities_) {
    if (c.id == id) return &c;
  }
  return nullptr;
}

void NodeService::startServer(StatusLed& led) {
  led_ = &led;
  server_.on("/reconclave/v1/announce", HTTP_GET, [this]() { handleAnnounce(); });
  server_.on("/reconclave/v1/message", HTTP_POST, [this]() { handleMessage(); });
  server_.onNotFound([this]() { server_.send(404, "application/json", "{\"error\":\"not found\"}"); });
  server_.begin();
  Serial.println("node: HTTP server up on /reconclave/v1/{announce,message}");
}

void NodeService::handleAnnounce() {
  JsonDocument doc;
  doc["proto"] = reconclave::kProtocolVersion;
  doc["type"] = "announce";
  doc["message_id"] = device_id_ + "-" + String((uint32_t)millis());
  doc["source_node"] = device_id_;
  doc["timestamp_ms"] = (uint64_t)millis();
  doc["sequence"] = sequence_++;

  JsonObject payload = doc["payload"].to<JsonObject>();
  payload["device_id"] = device_id_;
  payload["device_type"] = kDeviceType;
  payload["firmware"] = kFirmware;
  JsonArray roles = payload["roles"].to<JsonArray>();
  roles.add("node");

  JsonArray caps = payload["capabilities"].to<JsonArray>();
  JsonArray descriptors = payload["capability_descriptors"].to<JsonArray>();
  for (const auto& c : capabilities_) {
    caps.add(c.id);
    JsonObject d = descriptors.add<JsonObject>();
    d["id"] = c.id;
    d["permission"] = c.permission;
    JsonObject limits = d["limits"].to<JsonObject>();
    limits["weight"] = 1;
    limits["max_concurrency"] = 1;
  }

  JsonObject resources = payload["resources"].to<JsonObject>();
  resources["network_mbps"] = 20;
  resources["persistent_storage"] = false;
  payload["status"] = "ready";

  JsonObject security = payload["security"].to<JsonObject>();
  security["paired"] = true;
  security["mode"] = "provisioned-hmac-sha256-128";
  security["boot_nonce"] = trust_.bootNonceHex();

  String out;
  serializeJson(doc, out);
  server_.send(200, "application/json", out);
}

void NodeService::handleMessage() {
  const String body = server_.arg("plain");
  if (body.length() == 0 || body.length() > kMaxBodyBytes) {
    server_.send(400, "application/json", "{\"error\":\"empty or oversize body\"}");
    return;
  }

  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    server_.send(400, "application/json", "{\"error\":\"invalid JSON\"}");
    return;
  }
  if (String(doc["proto"] | "") != reconclave::kProtocolVersion ||
      String(doc["type"] | "") != "request") {
    server_.send(400, "application/json", "{\"error\":\"unsupported envelope\"}");
    return;
  }

  const String source = doc["source_node"] | "";
  const String destination = doc["destination_node"] | "";
  if (destination.length() != 0 && destination != device_id_) {
    server_.send(400, "application/json", "{\"error\":\"wrong destination\"}");
    return;
  }

  JsonObjectConst payload = doc["payload"].as<JsonObjectConst>();
  const String capability = payload["capability"] | "";
  const String request_id = payload["request_id"] | "";
  JsonVariantConst arguments = payload["arguments"];
  JsonObjectConst auth = payload["auth"].as<JsonObjectConst>();

  RequestContext ctx;
  ctx.source_node = source;
  ctx.request_id = request_id;
  ctx.capability = capability;
  ctx.authenticated = false;

  CapabilityOutcome outcome;
  const Registered* reg = find(capability);
  if (reg == nullptr) {
    outcome.status = "rejected";
    outcome.error_code = "CAPABILITY_UNAVAILABLE";
    outcome.error_message = "capability not offered by this node";
  } else {
    bool authed = true;
    if (reg->permission != "public") {
      String reason;
      const rc_provisioned_peer_t* peer =
          trust_.verifyRequest(source.c_str(), device_id_.c_str(), request_id.c_str(),
                               capability.c_str(), arguments, auth, reason);
      authed = peer != nullptr;
      if (!authed) {
        Serial.printf("node: request rejected (%s): %s\n", capability.c_str(), reason.c_str());
        outcome.status = "rejected";
        outcome.error_code = "UNAUTHORIZED";
        outcome.error_message = reason;
        if (led_) led_->flash(CRGB(255, 170, 28), 250);  // amber
      }
    }
    if (authed) {
      ctx.authenticated = true;
      outcome = reg->handler(arguments, ctx);
      if (led_) {
        led_->flash(outcome.status == "accepted" || outcome.status == "ok" ? CRGB::Green
                                                                           : CRGB(255, 170, 28),
                    250);
      }
    }
  }

  // Build the response envelope (unsigned in milestone 1; response signing is m2).
  JsonDocument resp;
  resp["proto"] = reconclave::kProtocolVersion;
  resp["type"] = "response";
  resp["message_id"] = device_id_ + "-" + String((uint32_t)millis());
  resp["source_node"] = device_id_;
  resp["destination_node"] = source;
  resp["timestamp_ms"] = (uint64_t)millis();
  JsonObject rp = resp["payload"].to<JsonObject>();
  rp["request_id"] = request_id;
  rp["status"] = outcome.status;
  if (outcome.error_code.length()) rp["error_code"] = outcome.error_code;
  if (outcome.error_message.length()) rp["error_message"] = outcome.error_message;
  JsonDocument result;
  if (deserializeJson(result, outcome.result_json) == DeserializationError::Ok) {
    rp["result"] = result.as<JsonVariantConst>();
  }

  String out;
  serializeJson(resp, out);
  server_.send(200, "application/json", out);
}

void NodeService::handleClient() { server_.handleClient(); }

}  // namespace reconclave
