#include "node_service.h"

#include <WiFi.h>

#include "reconclave/protocol.h"  // shared domain library (proves fleet integration)
#include "framework.h"            // Config
#include "radio_manager.h"
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

void NodeService::startServer(StatusLed& led, Config& config, RadioManager& radio) {
  led_ = &led;
  config_ = &config;
  radio_ = &radio;
  // Protocol surface.
  server_.on("/reconclave/v1/announce", HTTP_GET, [this]() { handleAnnounce(); });
  server_.on("/reconclave/v1/message", HTTP_POST, [this]() { handleMessage(); });
  // Setup/status web UI (no payload arming).
  server_.on("/", HTTP_GET, [this]() { handleRoot(); });
  server_.on("/admin/status", HTTP_GET, [this]() { handleAdminStatus(); });
  server_.on("/admin/wifi", HTTP_POST, [this]() { handleAdminWifi(); });
  server_.onNotFound([this]() { server_.send(404, "application/json", "{\"error\":\"not found\"}"); });
  server_.begin();
  Serial.println("node: HTTP server up (/, /admin/*, /reconclave/v1/*)");
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

namespace {
// Setup/status page. Brand palette (common/identity), deliberately setup-only:
// there is no payload control here — a fleet node's actions come from the
// coordinator under a signed scope, never a local button.
const char kSetupPage[] = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Reconclave node</title>
<style>:root{--cyan:#00cdd7;--navy:#0e222e;--raised:#16303f;--ink:#fff2d7;--muted:#c9b896;--warn:#ffaa1c}
*{box-sizing:border-box}body{margin:0;background:#081820;color:var(--ink);font:14px system-ui,sans-serif;padding:18px}
h1{font-size:16px;letter-spacing:.12em;color:var(--cyan);margin:0 0 4px}.sub{color:var(--muted);font-size:11px;margin-bottom:16px}
.card{background:var(--navy);border:1px solid var(--raised);border-radius:8px;padding:14px;margin-bottom:12px}
.row{display:flex;justify-content:space-between;gap:8px;padding:4px 0;border-bottom:1px solid var(--raised);font-size:12px}
.row:last-child{border:0}.row b{color:var(--muted);font-weight:500}label{display:block;font-size:11px;color:var(--muted);margin:8px 0 3px}
input{width:100%;background:#081820;border:1px solid var(--raised);color:var(--ink);border-radius:6px;padding:9px}
button{margin-top:12px;width:100%;background:var(--cyan);color:#04141b;border:0;border-radius:6px;padding:11px;font-weight:600;cursor:pointer}
.note{color:var(--warn);font-size:11px;margin-top:10px;line-height:1.4}.ok{color:var(--cyan)}</style></head>
<body><h1>RECONCLAVE NODE</h1><div class="sub" id="did">…</div>
<div class="card"><div class="row"><b>Wi-Fi</b><span id="link">…</span></div>
<div class="row"><b>STA IP</b><span id="staip">…</span></div><div class="row"><b>Setup AP</b><span id="ap">…</span></div>
<div class="row"><b>AP IP</b><span id="apip">…</span></div><div class="row"><b>Capabilities</b><span id="caps">…</span></div></div>
<div class="card"><b style="color:var(--cyan);font-size:12px">Wi-Fi setup</b>
<label>SSID</label><input id="s"><label>Password</label><input id="p" type="password">
<button onclick="save()">Save &amp; reboot to join</button>
<div class="note">This is a setup/status console only. Payloads run on this node only when the coordinator invokes them under a verified signed scope — there is deliberately no local run/arm control here.</div></div>
<script>async function r(){let d=await(await fetch('/admin/status')).json();did.textContent=d.device_id;
link.textContent=d.sta_link?'up':'down';link.className=d.sta_link?'ok':'';staip.textContent=d.sta_ip;
ap.textContent=d.ap_ssid;apip.textContent=d.ap_ip;caps.textContent=(d.capabilities||[]).join(', ')}
async function save(){await fetch('/admin/wifi',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({ssid:s.value,pass:p.value})});alert('Saved. The node is rebooting to join.')}r()</script>
</body></html>)HTML";
}  // namespace

void NodeService::handleRoot() {
  server_.send_P(200, "text/html; charset=utf-8", kSetupPage);
}

void NodeService::handleAdminStatus() {
  JsonDocument doc;
  doc["device_id"] = device_id_;
  doc["sta_link"] = radio_ != nullptr && radio_->staConnected();
  doc["sta_ip"] = radio_ != nullptr ? radio_->staIp().toString() : String("0.0.0.0");
  doc["ap_ssid"] = radio_ != nullptr ? radio_->apSsid() : String("");
  doc["ap_ip"] = radio_ != nullptr ? radio_->apIp().toString() : String("0.0.0.0");
  JsonArray caps = doc["capabilities"].to<JsonArray>();
  for (const auto& c : capabilities_) caps.add(c.id);
  String out;
  serializeJson(doc, out);
  server_.send(200, "application/json", out);
}

void NodeService::handleAdminWifi() {
  const String body = server_.arg("plain");
  JsonDocument doc;
  if (body.length() == 0 || deserializeJson(doc, body)) {
    server_.send(400, "application/json", "{\"error\":\"invalid JSON\"}");
    return;
  }
  const String ssid = doc["ssid"] | "";
  const String pass = doc["pass"] | "";
  if (ssid.length() == 0 || config_ == nullptr) {
    server_.send(400, "application/json", "{\"error\":\"ssid required\"}");
    return;
  }
  config_->setString("wifi_ssid", ssid);
  config_->setString("wifi_pass", pass);
  server_.send(200, "application/json", "{\"ok\":true,\"rebooting\":true}");
  Serial.printf("admin: wifi set to \"%s\" via web UI - rebooting to join\n", ssid.c_str());
  delay(300);
  ESP.restart();
}

void NodeService::handleClient() { server_.handleClient(); }

}  // namespace reconclave
