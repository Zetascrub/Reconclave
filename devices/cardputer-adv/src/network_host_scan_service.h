#pragma once

#include <Arduino.h>
#include <IPAddress.h>
#include <vector>
#include <atomic>

struct LocalHostResult { IPAddress ip; };

class LocalHostScanService {
 public:
  bool start(uint32_t maxHosts = 254);
  bool startRange(uint32_t firstHost, uint32_t hostCount);
  void stop();
  void update();
  bool nextResult(LocalHostResult& result);
  bool active() const { return active_; }
  uint32_t checked() const { return current_; }
  uint32_t total() const { return total_; }

 private:
  void beginNext();
  void finishCurrent();
  IPAddress candidate(uint32_t index) const;
  static void onSuccess(void* handle, void* argument);
  static void onEnd(void* handle, void* argument);

  bool active_{false};
  uint32_t network_{0};
  uint32_t firstHost_{1};
  uint32_t ownIp_{0};
  uint32_t total_{0};
  uint32_t current_{0};
  std::atomic<void*> handle_{nullptr};
  unsigned long startedMs_{0};
  std::atomic<bool> done_{false};
  std::atomic<bool> found_{false};
  std::vector<LocalHostResult> pending_;
};
