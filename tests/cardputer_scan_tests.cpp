#include "../devices/cardputer-adv/src/network_host_scan_service.h"
#include "../devices/cardputer-adv/src/network_port_scan_service.h"
#include "../devices/cardputer-adv/src/uptime.h"
#include <WiFi.h>
#include <ping/ping_sock.h>
#include <lwip/sockets.h>
#include <cassert>
#include <map>
#include <chrono>
#include <thread>

TestWifi WiFi;
unsigned long millis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::map<void*, esp_ping_callbacks_t> callbacks;
uintptr_t nextHandle = 1;
void* current = nullptr;
uint32_t target = 0;
bool failStart = false;
int esp_ping_new_session(const esp_ping_config_t* cfg, const esp_ping_callbacks_t* cb, void** handle) {
  *handle = current = reinterpret_cast<void*>(nextHandle++);
  callbacks[*handle] = *cb;
  target = cfg->target_addr;
  return ESP_OK;
}
int esp_ping_start(void*) { return failStart ? -1 : ESP_OK; }
int esp_ping_stop(void*) { return ESP_OK; }
int esp_ping_delete_session(void* handle) { callbacks.erase(handle); return ESP_OK; }
int boundSocket(uint16_t& port, bool listenSocket) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
  socklen_t size = sizeof(address);
  assert(getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) == 0);
  port = ntohs(address.sin_port);
  if (listenSocket) assert(listen(fd, 1) == 0);
  return fd;
}
int main() {
  using reconclave::uptimeDue;
  assert(!uptimeDue(0xfffffff0U, 0x10U));
  assert(uptimeDue(0x10U, 0x10U));
  assert(uptimeDue(0x20U, 0x10U));
  assert(uptimeDue(0x10U, 0xfffffff0U));
  LocalHostScanService host;
  assert(!host.startRange(1,0));
  assert(!host.startRange(0,1));
  assert(!host.startRange(255,1));
  assert(host.startRange(1,3));
  const auto oldHandle = current;
  const auto oldCallbacks = callbacks.at(current);
  assert(target == 0xc0a80101U);
  assert(!host.startRange(9,1)); // Cannot overwrite a live ping session.
  assert(callbacks.size() == 1 && host.total() == 3);
  oldCallbacks.on_ping_success(oldHandle, oldCallbacks.cb_args);
  oldCallbacks.on_ping_end(oldHandle, oldCallbacks.cb_args);
  host.update();
  assert(target == 0xc0a80103U); // Own address .2 is skipped.
  LocalHostResult result;
  assert(host.nextResult(result) && result.ip[3] == 1);
  assert(!host.nextResult(result));
  oldCallbacks.on_ping_success(oldHandle, oldCallbacks.cb_args);
  oldCallbacks.on_ping_end(oldHandle, oldCallbacks.cb_args);
  host.update();
  assert(host.active()); // A late callback must not finish the new target.
  const auto cb = callbacks.at(current);
  cb.on_ping_end(current, cb.cb_args);
  host.update();
  assert(!host.active() && !host.nextResult(result) && callbacks.empty());
  failStart = true;
  assert(host.startRange(4,1));
  assert(!host.active() && callbacks.empty());
  WiFi.mask = IPAddress(255,255,255,255);
  assert(!host.startRange(1,1));

  uint16_t ports[2];
  const int openFd = boundSocket(ports[0], true);
  const int closedFd = boundSocket(ports[1], false);
  NetworkPortScanService scanner;
  assert(scanner.start(IPAddress(127,0,0,1), ports, 2));
  const auto deadline = millis() + 3000;
  while (scanner.active() && millis() < deadline) {
    scanner.update();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  assert(!scanner.active() && scanner.checked() == 2);
  NetworkPortResult port;
  assert(scanner.nextResult(port) && port.port == ports[0]);
  assert(!scanner.nextResult(port));
  close(openFd);
  close(closedFd);
}
