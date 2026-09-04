#pragma once

#include "reconclave/protocol.h"

#include <cstdint>
#include <string>
#include <deque>
#include <vector>

namespace reconclave {

inline constexpr std::size_t kMaxDiscoveredNodes = 32;

struct NodeRecord {
  NodeAnnouncement announcement;
  std::uint64_t last_seen_ms{0};
};

class NodeRegistry {
 public:
  bool observe(const NodeAnnouncement& announcement, std::uint64_t now_ms);
  std::size_t expireBefore(std::uint64_t cutoff_ms);
  const NodeRecord* find(const std::string& device_id) const;
  std::vector<const NodeRecord*> providersOf(const std::string& capability) const;
  std::size_t size() const { return nodes_.size(); }

 private:
  // Discovery inserts must not invalidate pointers returned for other live nodes.
  std::deque<NodeRecord> nodes_;
};

}  // namespace reconclave
