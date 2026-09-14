// The fleet-node protocol surface: serves the mDNS-discovered HTTP endpoints
//   GET  /reconclave/v1/announce   -> this node's NodeAnnouncement + security
//   POST /reconclave/v1/message    -> a protocol envelope (request today)
// authenticates requests via Trust, and dispatches to the capability handler a
// module registered. Modules never touch HTTP or crypto; they register a
// handler and receive already-authenticated arguments.
//
// Milestone 1: request authentication (HMAC) and dispatch are implemented;
// response signing and signed-scope-delegation verification are milestone 2
// (until then a trusted action fails closed in its own handler).
#pragma once

#include <ArduinoJson.h>
#include <WebServer.h>

#include <functional>
#include <vector>

#include "trust.h"

namespace reconclave {

class Config;
class RadioManager;
class StatusLed;

struct RequestContext {
  String source_node;
  String request_id;
  String capability;
  bool authenticated{false};
};

struct CapabilityOutcome {
  String status{"rejected"};  // "accepted" | "ok" | "rejected" | "error"
  String error_code;
  String error_message;
  String result_json{"{}"};
};

using CapabilityHandler = std::function<CapabilityOutcome(JsonVariantConst args,
                                                          const RequestContext& ctx)>;

class NodeService {
 public:
  // Compute the MAC-derived device id and a fresh boot nonce (no network yet).
  void beginIdentity(Config& config);
  // Wire the HTTP routes and start the server (call once the network is up).
  // Serves the reconclave/1 protocol endpoints AND a setup/status-only web UI
  // (WiFi config, status/announce view) reachable via the management AP or STA.
  // The web UI deliberately exposes no payload arming — actions on a fleet node
  // come only from the coordinator under a signed scope.
  void startServer(StatusLed& led, Config& config, RadioManager& radio);

  void registerCapability(const String& id, const String& permission, CapabilityHandler handler);
  void handleClient();

  const String& deviceId() const { return device_id_; }
  Trust& trust() { return trust_; }

 private:
  struct Registered {
    String id;
    String permission;
    CapabilityHandler handler;
  };

  void handleAnnounce();
  void handleMessage();
  void handleRoot();          // setup/status web UI (HTML)
  void handleAdminStatus();   // status JSON for the web UI
  void handleAdminWifi();     // POST {ssid,pass}: store to NVS + reboot
  const Registered* find(const String& id) const;

  Trust trust_;
  WebServer server_{80};
  StatusLed* led_ = nullptr;
  Config* config_ = nullptr;
  RadioManager* radio_ = nullptr;
  String device_id_;
  uint64_t sequence_ = 0;
  std::vector<Registered> capabilities_;
};

}  // namespace reconclave
