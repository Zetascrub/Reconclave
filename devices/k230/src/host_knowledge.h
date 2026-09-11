#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "discovery_job.h"
#include "json.h"
#include "network_inventory.h"
#include "network_tools.h"

namespace reconclave {

struct KnownService {
  std::uint16_t port = 0;
  std::string service;
  std::string banner;
  std::string attention;
  std::uint64_t last_seen_ms = 0;
};

struct HostRecord {
  std::string address;
  std::string mac;
  std::string interface;
  std::vector<std::uint16_t> open_ports;
  std::vector<KnownService> services;
  std::uint64_t first_seen_ms = 0;
  std::uint64_t last_seen_ms = 0;
  std::uint64_t observations = 0;
  std::uint64_t changes = 0;
};

class HostKnowledge {
 public:
  explicit HostKnowledge(std::string path);

  bool load(std::string& error);
  bool save(std::string& error) const;
  unsigned observeArp(const std::vector<ArpNeighbour>& neighbours, std::uint64_t now_ms);
  unsigned observeDiscovery(const std::vector<DiscoveredHost>& hosts, std::uint64_t now_ms);
  unsigned observeService(const ServiceObservation& service, std::uint64_t now_ms);
  std::vector<HostRecord> snapshot() const;
  json::Value toJson() const;
  json::Value findingsJson() const;
  std::size_t attentionCount() const;
  const std::string& path() const { return path_; }

 private:
  HostRecord& touch(const std::string& address, std::uint64_t now_ms);
  std::string path_;
  std::map<std::string, HostRecord> hosts_;
};

}  // namespace reconclave
