#include "reconclave/node_registry.h"

#include <algorithm>

namespace reconclave {

bool NodeRegistry::observe(const NodeAnnouncement& announcement, std::uint64_t now_ms) {
  if (!validate(announcement).valid || now_ms == 0) return false;
  auto existing = std::find_if(nodes_.begin(), nodes_.end(), [&](const NodeRecord& node) {
    return node.announcement.device_id == announcement.device_id;
  });
  if (existing == nodes_.end()) {
    if (nodes_.size() >= kMaxDiscoveredNodes) return false;
    nodes_.push_back({announcement, now_ms});
  } else {
    if (now_ms < existing->last_seen_ms) return false;
    if (existing->announcement.device_type != announcement.device_type) return false;
    existing->announcement = announcement;
    existing->last_seen_ms = now_ms;
  }
  return true;
}

std::size_t NodeRegistry::expireBefore(std::uint64_t cutoff_ms) {
  const auto previous_size = nodes_.size();
  nodes_.erase(std::remove_if(nodes_.begin(), nodes_.end(), [&](const NodeRecord& node) {
    return node.last_seen_ms < cutoff_ms;
  }), nodes_.end());
  return previous_size - nodes_.size();
}

const NodeRecord* NodeRegistry::find(const std::string& device_id) const {
  const auto found = std::find_if(nodes_.begin(), nodes_.end(), [&](const NodeRecord& node) {
    return node.announcement.device_id == device_id;
  });
  return found == nodes_.end() ? nullptr : &*found;
}

std::vector<const NodeRecord*> NodeRegistry::providersOf(const std::string& capability) const {
  std::vector<const NodeRecord*> providers;
  if (!isValidCapability(capability)) return providers;
  for (const auto& node : nodes_) {
    const auto& capabilities = node.announcement.capabilities;
    if (std::find(capabilities.begin(), capabilities.end(), capability) != capabilities.end()) {
      providers.push_back(&node);
    }
  }
  return providers;
}

}  // namespace reconclave
