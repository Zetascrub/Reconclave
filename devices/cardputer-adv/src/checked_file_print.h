#pragma once
#include <FS.h>

// Print's printf/println can return a positive short write without setting the
// File error flag. Track each underlying write so a full SD card isn't "Saved".
class CheckedFilePrint : public Print {
 public:
  explicit CheckedFilePrint(File& file) : file_(file) {}
  size_t write(uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t* data, size_t size) override {
    const size_t written = file_.write(data, size);
    ok_ = ok_ && written == size;
    return written;
  }
  bool finish() {
    file_.flush();
    return ok_ && file_.getWriteError() == 0;
  }
 private:
  File& file_;
  bool ok_{true};
};
