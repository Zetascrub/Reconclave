#pragma once

#include <Arduino.h>
#include <IPAddress.h>
#include <vector>

struct NetworkPortResult { uint16_t port; };

class NetworkPortScanService {
 public:
  bool start(IPAddress target, const uint16_t* ports, size_t portCount);
  void stop();
  void update();
  bool active() const { return active_; }
  uint32_t checked() const { return checked_; }
  uint32_t total() const { return total_; }
  bool nextResult(NetworkPortResult& result);

 private:
  static constexpr size_t kSlots = 8;
  static constexpr uint32_t kTimeoutMs = 250;
  struct Slot {
    int fd{-1};
    uint16_t port{0};
    unsigned long started{0};
    bool used{false};
  };
  void fill();
  void poll();
  void closeSlot(size_t index);
  Slot slots_[kSlots];
  IPAddress target_;
  const uint16_t* ports_{nullptr};
  size_t portCount_{0};
  size_t next_{0};
  uint32_t checked_{0};
  uint32_t total_{0};
  bool active_{false};
  std::vector<NetworkPortResult> pending_;
};
