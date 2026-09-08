#pragma once
#include <cstdint>
#include <cstddef>
#include <algorithm>
class Print {
 public:
  virtual ~Print() = default;
  virtual size_t write(uint8_t) = 0;
  virtual size_t write(const uint8_t*, size_t) = 0;
};
class File {
 public:
  size_t allowance = 1024;
  size_t written = 0;
  bool flushed = false;
  int error = 0;
  size_t write(const uint8_t*, size_t count) {
    const auto size = std::min(count, allowance);
    written += size;
    allowance -= size;
    return size;
  }
  void flush() { flushed = true; }
  int getWriteError() const { return error; }
};
