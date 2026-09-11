#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace reconclave {

struct DiscoveredHost {
  std::string address;
  std::vector<std::uint16_t> open_ports;
};

struct DiscoverySnapshot {
  std::string job_id;
  std::string status{"idle"};
  unsigned checked = 0;
  unsigned total = 0;
  std::vector<DiscoveredHost> hosts;
  std::string error;
};

class DiscoveryJob {
 public:
  ~DiscoveryJob();
  bool start(const std::string& network, std::string& error);
  void cancel();
  DiscoverySnapshot snapshot() const;

 private:
  void run(std::uint32_t first, std::uint32_t last);
  mutable std::mutex mutex_;
  std::thread thread_;
  std::atomic<bool> cancel_{false};
  DiscoverySnapshot state_;
};

bool boundedIpv4Range(const std::string& network, std::uint32_t& first,
                      std::uint32_t& last, std::string& error);

}  // namespace reconclave
