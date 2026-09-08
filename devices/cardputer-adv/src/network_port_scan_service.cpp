#include "network_port_scan_service.h"

#include <lwip/sockets.h>

bool NetworkPortScanService::start(IPAddress target, const uint16_t* ports,
                                   size_t portCount) {
  stop();
  if (ports == nullptr || portCount == 0) return false;
  target_ = target;
  ports_ = ports;
  portCount_ = portCount;
  next_ = checked_ = 0;
  total_ = portCount;
  pending_.clear();
  active_ = true;
  fill();
  return true;
}

void NetworkPortScanService::stop() {
  for (size_t i = 0; i < kSlots; ++i) closeSlot(i);
  active_ = false;
}

void NetworkPortScanService::closeSlot(size_t index) {
  if (slots_[index].fd >= 0) close(slots_[index].fd);
  slots_[index] = Slot{};
}

void NetworkPortScanService::fill() {
  for (auto& slot : slots_) {
    if (slot.used || next_ >= portCount_) continue;
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    const uint16_t port = ports_[next_++];
    if (fd < 0) { ++checked_; continue; }
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
      close(fd);
      ++checked_;
      continue;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = static_cast<uint32_t>(target_);
    const int result = connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    if (result != 0 && errno != EINPROGRESS) {
      close(fd);
      ++checked_;
      continue;
    }
    slot = {fd, port, millis(), true};
  }
}

void NetworkPortScanService::poll() {
  fd_set writeSet;
  fd_set errorSet;
  FD_ZERO(&writeSet);
  FD_ZERO(&errorSet);
  int maxFd = -1;
  for (const auto& slot : slots_) {
    if (!slot.used) continue;
    FD_SET(slot.fd, &writeSet);
    FD_SET(slot.fd, &errorSet);
    maxFd = std::max(maxFd, slot.fd);
  }
  int readyCount = 0;
  if (maxFd >= 0) {
    timeval timeout{0, 0};
    readyCount = select(maxFd + 1, nullptr, &writeSet, &errorSet, &timeout);
  }
  const unsigned long now = millis();
  for (size_t i = 0; i < kSlots; ++i) {
    auto& slot = slots_[i];
    if (!slot.used) continue;
    const bool ready = readyCount > 0 &&
        (FD_ISSET(slot.fd, &writeSet) || FD_ISSET(slot.fd, &errorSet));
    if (!ready && now - slot.started <= kTimeoutMs) continue;
    if (ready) {
      int error = 0;
      socklen_t length = sizeof(error);
      if (getsockopt(slot.fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0)
        pending_.push_back({slot.port});
    }
    closeSlot(i);
    ++checked_;
  }
}

void NetworkPortScanService::update() {
  if (!active_) return;
  poll();
  fill();
  bool occupied = false;
  for (const auto& slot : slots_) occupied = occupied || slot.used;
  if (next_ >= portCount_ && !occupied) active_ = false;
}

bool NetworkPortScanService::nextResult(NetworkPortResult& result) {
  if (pending_.empty()) return false;
  result = pending_.front();
  pending_.erase(pending_.begin());
  return true;
}
