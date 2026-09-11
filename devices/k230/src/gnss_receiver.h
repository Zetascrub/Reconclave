#pragma once

#include <string>

namespace reconclave {

struct GnssFix {
  bool valid = false;
  double latitude = 0.0;
  double longitude = 0.0;
  double altitude_m = 0.0;
  double speed_knots = 0.0;
  int satellites = 0;
  std::string utc;
  std::string last_sentence;
};

// Parses a checksum-valid GGA or RMC sentence. Other valid NMEA messages are
// accepted but do not change the fix.
bool parseNmeaSentence(const std::string& sentence, GnssFix& fix);

class GnssReceiver {
 public:
  ~GnssReceiver();
  bool start(const std::string& device, std::string& error);
  void stop();
  void poll();
  bool running() const { return fd_ >= 0; }
  const GnssFix& fix() const { return fix_; }
  const std::string& status() const { return status_; }

 private:
  bool sendCommand(const char* command);
  int fd_ = -1;
  std::string buffer_;
  std::string status_{"GNSS idle"};
  GnssFix fix_;
};

}  // namespace reconclave
