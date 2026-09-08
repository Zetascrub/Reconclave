#pragma once
#include <cstdint>
class IPAddress {
 public:
  IPAddress(uint8_t a=0, uint8_t b=0, uint8_t c=0, uint8_t d=0) : bytes_{a,b,c,d} {}
  uint8_t operator[](unsigned index) const { return bytes_[index]; }
  operator uint32_t() const {
    return uint32_t(bytes_[0]) | uint32_t(bytes_[1]) << 8 |
        uint32_t(bytes_[2]) << 16 | uint32_t(bytes_[3]) << 24;
  }
 private:
  uint8_t bytes_[4];
};
