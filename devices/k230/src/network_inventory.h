#pragma once

#include <string>
#include <vector>

namespace reconclave {

struct ArpNeighbour {
  std::string address;
  std::string mac;
  std::string interface;
  bool complete = false;
};

// Reads the kernel neighbour cache without transmitting packets. This is safe
// for local UI refreshes and the read-only net.arp.snapshot capability.
bool readArpSnapshot(std::vector<ArpNeighbour>& out, std::string& error,
                     const std::string& path = "/proc/net/arp");

}  // namespace reconclave
