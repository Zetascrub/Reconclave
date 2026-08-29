#include "network_host_scan_service.h"

#include <WiFi.h>
#include <algorithm>
#include <ping/ping_sock.h>

namespace {
constexpr uint32_t kPingTimeoutMs = 180;

uint32_t ipValue(const IPAddress& ip) {
  return (static_cast<uint32_t>(ip[0]) << 24) |
      (static_cast<uint32_t>(ip[1]) << 16) |
      (static_cast<uint32_t>(ip[2]) << 8) | ip[3];
}

IPAddress fromValue(uint32_t value) {
  return IPAddress(value >> 24, value >> 16, value >> 8, value);
}
}  // namespace

IPAddress LocalHostScanService::candidate(uint32_t index) const {
  return fromValue(network_ + firstHost_ + index);
}

bool LocalHostScanService::start(uint32_t maxHosts) {
  return startRange(1, maxHosts);
}

bool LocalHostScanService::startRange(uint32_t firstHost, uint32_t hostCount) {
  if (WiFi.status() != WL_CONNECTED) return false;
  const uint32_t address = ipValue(WiFi.localIP());
  const uint32_t mask = ipValue(WiFi.subnetMask());
  network_ = address & mask;
  const uint32_t broadcast = network_ | ~mask;
  const uint32_t available = broadcast > network_ + 1
      ? broadcast - network_ - 1 : 0;
  ownIp_ = address;
  if (firstHost == 0 || firstHost > available) return false;
  firstHost_ = firstHost;
  total_ = std::min(available - firstHost + 1, hostCount);
  current_ = 0;
  pending_.clear();
  active_ = true;
  beginNext();
  return true;
}

void LocalHostScanService::stop() {
  if (handle_ != nullptr) {
    esp_ping_stop(handle_);
    esp_ping_delete_session(handle_);
    handle_ = nullptr;
  }
  active_ = false;
}

void LocalHostScanService::beginNext() {
  while (current_ < total_) {
    if (ipValue(candidate(current_)) == ownIp_) {
      ++current_;
      continue;
    }
    done_ = false;
    found_ = false;
    const IPAddress target = candidate(current_);
    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    config.count = 1;
    config.timeout_ms = kPingTimeoutMs;
    config.interval_ms = kPingTimeoutMs;
    config.data_size = 24;
    IP_ADDR4(&config.target_addr, target[0], target[1], target[2], target[3]);
    esp_ping_callbacks_t callbacks{};
    callbacks.cb_args = this;
    callbacks.on_ping_success = onSuccess;
    callbacks.on_ping_end = onEnd;
    esp_ping_handle_t handle = nullptr;
    if (esp_ping_new_session(&config, &callbacks, &handle) != ESP_OK) {
      ++current_;
      continue;
    }
    handle_ = handle;
    startedMs_ = millis();
    esp_ping_start(handle);
    return;
  }
  active_ = false;
}

void LocalHostScanService::finishCurrent() {
  if (found_) pending_.push_back({candidate(current_)});
  if (handle_ != nullptr) {
    esp_ping_stop(handle_);
    esp_ping_delete_session(handle_);
    handle_ = nullptr;
  }
  ++current_;
  beginNext();
}

void LocalHostScanService::update() {
  if (!active_) return;
  if (done_ || millis() - startedMs_ > kPingTimeoutMs + 750) finishCurrent();
}

bool LocalHostScanService::nextResult(LocalHostResult& result) {
  if (pending_.empty()) return false;
  result = pending_.front();
  pending_.erase(pending_.begin());
  return true;
}

void LocalHostScanService::onSuccess(void*, void* argument) {
  static_cast<LocalHostScanService*>(argument)->found_ = true;
}

void LocalHostScanService::onEnd(void*, void* argument) {
  static_cast<LocalHostScanService*>(argument)->done_ = true;
}
