#include "gnss_receiver.h"

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <poll.h>

#include <cmath>
#include <cstdlib>
#include <sstream>
#include <vector>

namespace reconclave {
namespace {

std::vector<std::string> split(const std::string& value, char delimiter) {
  std::vector<std::string> fields;
  std::istringstream input(value);
  std::string field;
  while (std::getline(input, field, delimiter)) fields.push_back(field);
  return fields;
}

bool checksumValid(const std::string& value) {
  if (value.empty() || value[0] != '$') return false;
  const auto star = value.find('*');
  if (star == std::string::npos || star + 2 >= value.size()) return false;
  unsigned checksum = 0;
  for (std::size_t i = 1; i < star; ++i) checksum ^= static_cast<unsigned char>(value[i]);
  char* end = nullptr;
  const unsigned expected = static_cast<unsigned>(std::strtoul(value.substr(star + 1, 2).c_str(), &end, 16));
  return end != nullptr && *end == '\0' && checksum == expected;
}

bool coordinate(const std::string& raw, const std::string& hemisphere, double& out) {
  if (raw.empty() || hemisphere.empty()) return false;
  char* end = nullptr;
  const double packed = std::strtod(raw.c_str(), &end);
  if (end == raw.c_str() || *end != '\0') return false;
  const double degrees = std::floor(packed / 100.0);
  out = degrees + (packed - degrees * 100.0) / 60.0;
  if (hemisphere == "S" || hemisphere == "W") out = -out;
  return hemisphere == "N" || hemisphere == "S" || hemisphere == "E" || hemisphere == "W";
}

}  // namespace

bool parseNmeaSentence(const std::string& sentence, GnssFix& fix) {
  if (!checksumValid(sentence)) return false;
  const auto star = sentence.find('*');
  const auto fields = split(sentence.substr(1, star - 1), ',');
  if (fields.empty()) return true;
  const std::string type = fields[0].size() >= 3 ? fields[0].substr(fields[0].size() - 3) : fields[0];
  if (type == "GGA" && fields.size() >= 10) {
    double latitude = 0.0, longitude = 0.0;
    const bool positioned = coordinate(fields[2], fields[3], latitude) &&
                            coordinate(fields[4], fields[5], longitude);
    fix.valid = positioned && std::atoi(fields[6].c_str()) > 0;
    if (positioned) { fix.latitude = latitude; fix.longitude = longitude; }
    fix.utc = fields[1];
    fix.satellites = std::atoi(fields[7].c_str());
    fix.altitude_m = std::strtod(fields[9].c_str(), nullptr);
    fix.last_sentence = sentence;
  } else if (type == "RMC" && fields.size() >= 8) {
    double latitude = 0.0, longitude = 0.0;
    const bool positioned = coordinate(fields[3], fields[4], latitude) &&
                            coordinate(fields[5], fields[6], longitude);
    fix.valid = fields[2] == "A" && positioned;
    if (positioned) { fix.latitude = latitude; fix.longitude = longitude; }
    fix.utc = fields[1];
    fix.speed_knots = std::strtod(fields[7].c_str(), nullptr);
    fix.last_sentence = sentence;
  }
  return true;
}

GnssReceiver::~GnssReceiver() { stop(); }

bool GnssReceiver::sendCommand(const char* command) {
  std::string wire = std::string(command) + "\r\n";
  return write(fd_, wire.data(), wire.size()) == static_cast<ssize_t>(wire.size());
}

void GnssReceiver::processLine(const std::string& raw) {
  std::string line = raw;
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
  if (line.empty()) return;
  last_response_ = line;
  const char* position_prefix = line.find("#XGNSSPOS:") != std::string::npos
      ? "#XGNSSPOS:" : line.find("#XGPSPOS:") != std::string::npos ? "#XGPSPOS:" : nullptr;
  const auto fix_position = position_prefix == nullptr ? std::string::npos : line.find(position_prefix);
  if (fix_position != std::string::npos) {
    const auto fields = split(line.substr(fix_position + std::char_traits<char>::length(position_prefix)), ',');
    if (fields.size() >= 7) {
      char* end = nullptr;
      const double latitude = std::strtod(fields[0].c_str(), &end);
      const bool latitude_valid = end != fields[0].c_str();
      const double longitude = std::strtod(fields[1].c_str(), &end);
      const bool longitude_valid = end != fields[1].c_str();
      if (latitude_valid && longitude_valid) {
        fix_.valid = true;
        fix_.latitude = latitude;
        fix_.longitude = longitude;
        fix_.altitude_m = std::strtod(fields[2].c_str(), nullptr);
        fix_.speed_knots = std::strtod(fields[4].c_str(), nullptr) * 1.943844;
        fix_.utc = fields[6];
        while (!fix_.utc.empty() && (fix_.utc.front() == ' ' || fix_.utc.front() == '"'))
          fix_.utc.erase(0, 1);
        while (!fix_.utc.empty() && fix_.utc.back() == '"') fix_.utc.pop_back();
        fix_.last_sentence = line;
        status_ = "GNSS fix acquired";
      }
    }
    return;
  }
  constexpr char prefix[] = "#XGNSSNMEA:";
  const auto position = line.find(prefix);
  if (position == std::string::npos) return;
  auto sentence = line.substr(position + sizeof(prefix) - 1);
  while (!sentence.empty() && sentence.front() == ' ') sentence.erase(0, 1);
  ++nmea_count_;
  if (parseNmeaSentence(sentence, fix_))
    status_ = fix_.valid ? "GNSS fix acquired" : "GNSS active; acquiring fix";
}

bool GnssReceiver::exchange(const char* command, int timeout_ms, std::string& response) {
  response.clear();
  if (!sendCommand(command)) return false;
  const int slices = timeout_ms / 50 + 1;
  std::string reply;
  for (int i = 0; i < slices; ++i) {
    pollfd descriptor{fd_, POLLIN, 0};
    if (::poll(&descriptor, 1, 50) <= 0) continue;
    char chunk[256];
    const ssize_t count = read(fd_, chunk, sizeof(chunk));
    if (count <= 0) continue;
    reply.append(chunk, static_cast<std::size_t>(count));
    std::size_t newline;
    while ((newline = reply.find('\n')) != std::string::npos) {
      std::string line = reply.substr(0, newline);
      reply.erase(0, newline + 1);
      while (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty() || line == command) continue;
      processLine(line);
      if (!response.empty()) response += " | ";
      response += line;
      if (line == "OK") return true;
      if (line == "ERROR" || line.find("+CME ERROR") != std::string::npos) return false;
    }
  }
  if (!reply.empty()) {
    processLine(reply);
    if (!response.empty()) response += " | ";
    response += reply;
  }
  return false;
}

bool GnssReceiver::start(const std::string& device, std::string& error) {
  stop();
  last_response_.clear();
  modem_info_.clear();
  api_info_.clear();
  fd_ = open(device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd_ < 0) { error = "unable to open modem UART"; return false; }
  termios tty{};
  if (tcgetattr(fd_, &tty) != 0) { error = "unable to configure modem UART"; stop(); return false; }
  cfmakeraw(&tty);
  cfsetispeed(&tty, B115200);
  cfsetospeed(&tty, B115200);
  tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8 | CLOCAL | CREAD;
  tty.c_cflag &= ~(PARENB | CSTOPB | CRTSCTS);
  if (tcsetattr(fd_, TCSANOW, &tty) != 0) { error = "unable to apply modem UART settings"; stop(); return false; }
  tcflush(fd_, TCIOFLUSH);
  fix_ = GnssFix{};
  nmea_count_ = 0;
  std::string response;
  if (!exchange("AT", 1500, response)) {
    error = "nRF9151 did not answer AT" + (response.empty() ? std::string() : ": " + response);
    const std::string diagnosis = error;
    stop(); status_ = "GNSS error: " + diagnosis; return false;
  }
  std::string identity;
  if (exchange("ATI", 2000, identity)) modem_info_ = identity;
  std::string revision;
  if (exchange("AT+CGMR", 2000, revision)) {
    if (!modem_info_.empty()) modem_info_ += " | ";
    modem_info_ += revision;
  }
  std::string modern_test, legacy_test;
  const bool modern_supported = exchange("AT#XGNSS=?", 2000, modern_test);
  const bool legacy_supported = exchange("AT#XGPS=?", 2000, legacy_test);
  api_info_ = "XGNSS=" + modern_test + " | XGPS=" + legacy_test;
  if (!modern_supported && !legacy_supported) {
    error = "installed nRF9151 application has no XGNSS/XGPS service";
    const std::string diagnosis = error;
    stop(); status_ = "GNSS unavailable: " + diagnosis; return false;
  }
  // Recover to offline first, then use the exact smoke-test order required by
  // LilyGO's K230 Serial LTE Modem build. XNMEA must be configured before
  // CFUN=31; that custom command is rejected once this firmware is online.
  exchange("AT#XGNSS=0", 1500, response);
  exchange("AT#XNMEA=0", 1500, response);
  const char* rejected_command = nullptr;
  if (!exchange("AT+CFUN=0", 6000, response))
    rejected_command = "AT+CFUN=0";
  const bool nmea_enabled = exchange("AT#XNMEA=1", 3000, response);
  if (!exchange("AT%XSYSTEMMODE=0,0,1,0", 3000, response))
    rejected_command = "AT%XSYSTEMMODE=0,0,1,0";
  else if (!exchange("AT+CFUN=31", 9000, response))
    rejected_command = "AT+CFUN=31";
  if (rejected_command != nullptr) {
    error = std::string("GNSS setup rejected ") + rejected_command + ": " + response;
    const std::string diagnosis = error;
    stop(); status_ = "GNSS error: " + diagnosis; return false;
  }
  bool legacy_gps = false;
  if ((!modern_supported || !exchange("AT#XGNSS=1,0,1", 3000, response)) &&
      (!modern_supported || !exchange("AT#XGNSS=1,0,0,0", 3000, response))) {
    std::string state;
    const bool modern_active = exchange("AT#XGNSS?", 2000, state) &&
        state.find("#XGNSS: 1") != std::string::npos;
    if (!modern_active) {
      // Nordic Serial LTE Modem releases through NCS 3.1 called this API XGPS.
      legacy_gps = legacy_supported && exchange("AT#XGPS=1,0,0,0", 3000, response);
      if (legacy_supported && !legacy_gps) {
        // Some nRF9151/SLM combinations advertise the legacy API but reject
        // its standalone CFUN=31 path. Retry in the documented concurrent
        // LTE-M + GNSS operating mode before declaring the service unusable.
        std::string recovery;
        if (exchange("AT+CFUN=0", 6000, recovery) &&
            exchange("AT%XSYSTEMMODE=1,0,1,0", 3000, recovery) &&
            exchange("AT+CFUN=1", 9000, recovery)) {
          legacy_gps = exchange("AT#XGPS=1,0,0,0", 3000, response);
        }
      }
    }
    if (!modern_active && !legacy_gps) {
      error = "GNSS/XGPS start rejected: " + response;
      const std::string diagnosis = error;
      stop(); status_ = "GNSS error: " + diagnosis; return false;
    }
  }
  status_ = nmea_enabled ? "GNSS started; waiting for satellite fix" :
      legacy_gps ? "Legacy GPS active; waiting for position report" :
      "GNSS active; waiting for position report";
  error.clear();
  return true;
}

void GnssReceiver::stop() {
  if (fd_ >= 0) close(fd_);
  fd_ = -1;
  buffer_.clear();
  status_ = "GNSS idle";
}

void GnssReceiver::poll() {
  if (fd_ < 0) return;
  char chunk[512];
  ssize_t count = 0;
  while ((count = read(fd_, chunk, sizeof(chunk))) > 0) buffer_.append(chunk, static_cast<std::size_t>(count));
  std::size_t newline = 0;
  while ((newline = buffer_.find('\n')) != std::string::npos) {
    std::string line = buffer_.substr(0, newline);
    buffer_.erase(0, newline + 1);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    processLine(line);
  }
  if (buffer_.size() > 4096) buffer_.erase(0, buffer_.size() - 4096);
}

}  // namespace reconclave
