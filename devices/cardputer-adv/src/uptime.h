#pragma once
#include <cstdint>
namespace reconclave {
// Deadlines must be less than 2^31 ms ahead; survives the 49.7-day millis wrap.
inline bool uptimeDue(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}
}
