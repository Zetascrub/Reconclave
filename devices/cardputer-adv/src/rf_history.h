#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace cap {
// 20 seconds at 10 Hz. Each band owns its history, so switching never joins
// unrelated frequencies into a single trace. Oldest samples appear on the left.
class RfHistory {
 public:
  static constexpr size_t capacity = 200;
  bool push(float dbm, uint32_t sampledAtMs = 0) {
    if (!std::isfinite(dbm)) return false;
    values_[next_] = dbm;
    times_[next_] = sampledAtMs;
    next_ = (next_ + 1) % capacity;
    if (count_ < capacity) ++count_;
    return true;
  }
  size_t size() const { return count_; }
  float at(size_t index) const {
    if (index >= count_) return -120.0f;
    return values_[(next_ + capacity - count_ + index) % capacity];
  }
  uint32_t sampledAt(size_t index) const {
    return index < count_ ? times_[(next_ + capacity - count_ + index) % capacity] : 0;
  }
  void clear() { next_ = count_ = 0; }
  unsigned activityPercent(float threshold) const {
    if (!count_ || !std::isfinite(threshold)) return 0;
    size_t active = 0;
    for (size_t i = 0; i < count_; ++i) if (at(i) >= threshold) ++active;
    return static_cast<unsigned>((active * 100 + count_ / 2) / count_);
  }
  bool connectedToPrevious(size_t index) const {
    return index > 0 && index < count_ &&
        static_cast<uint32_t>(sampledAt(index) - sampledAt(index - 1)) <= 250;
  }
  static int height(float dbm, int pixels) {
    if (!std::isfinite(dbm) || pixels <= 0) return 0;
    const float level = (dbm + 120.0f) / 90.0f; // Fixed -120 to -30 dBm scale.
    return level <= 0 ? 0 : level >= 1 ? pixels : static_cast<int>(level * pixels);
  }
 private:
  std::array<float, capacity> values_{};
  std::array<uint32_t, capacity> times_{};
  size_t next_{0}, count_{0};
};
}
