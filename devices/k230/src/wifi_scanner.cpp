#include "wifi_scanner.h"

#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <algorithm>

namespace reconclave {

namespace {

// Runs `argv` (no shell involved) and returns its captured stdout. Returns
// false with `error` set on a spawn/exec/wait failure or non-zero exit
// status; stdout captured so far is still returned in `out` either way,
// since a partial/empty scan result is more useful to a caller than
// nothing.
bool runCapture(const std::vector<std::string>& argv, std::string& out, std::string& error) {
  int pipe_fds[2];
  if (pipe(pipe_fds) != 0) {
    error = "pipe() failed";
    return false;
  }

  pid_t pid = fork();
  if (pid < 0) {
    error = "fork() failed";
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    return false;
  }

  if (pid == 0) {
    // Child: stdout -> pipe write end, then exec. No shell involved, so
    // none of argv is ever interpreted - safe even if a future caller
    // passes a less-trusted interface name.
    dup2(pipe_fds[1], STDOUT_FILENO);
    close(pipe_fds[0]);
    close(pipe_fds[1]);

    std::vector<char*> exec_argv;
    exec_argv.reserve(argv.size() + 1);
    for (const auto& arg : argv) exec_argv.push_back(const_cast<char*>(arg.c_str()));
    exec_argv.push_back(nullptr);

    execvp(exec_argv[0], exec_argv.data());
    _exit(127);  // execvp only returns on failure.
  }

  // Parent.
  close(pipe_fds[1]);
  char buffer[4096];
  ssize_t n;
  while ((n = read(pipe_fds[0], buffer, sizeof(buffer))) > 0) {
    out.append(buffer, static_cast<std::size_t>(n));
  }
  close(pipe_fds[0]);

  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    error = "waitpid() failed";
    return false;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    error = "command exited with status " + std::to_string(status);
    return false;
  }
  return true;
}

std::string trim(const std::string& s) {
  std::size_t start = s.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) return "";
  std::size_t end = s.find_last_not_of(" \t\r\n");
  return s.substr(start, end - start + 1);
}

void flushObservation(WifiObservation& current, bool& has_current,
                       std::vector<WifiObservation>& out) {
  if (has_current) out.push_back(current);
  current = WifiObservation{};
  has_current = false;
}

// A radio interface reachable at all (out of reach of a real deployment's
// operator) has to bring itself up before it can scan - confirmed
// necessary on this board, whose wlan0 starts DOWN on every boot. Direct
// ioctl rather than shelling out to ip/ifconfig again, since this is a
// one-line syscall and it keeps the scanner from depending on either tool
// being present.
void ensureInterfaceUp(const std::string& interface) {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return;

  ifreq request{};
  std::strncpy(request.ifr_name, interface.c_str(), IFNAMSIZ - 1);
  if (ioctl(fd, SIOCGIFFLAGS, &request) == 0 && !(request.ifr_flags & IFF_UP)) {
    request.ifr_flags |= IFF_UP;
    ioctl(fd, SIOCSIFFLAGS, &request);
  }
  close(fd);
}

}  // namespace

bool scanWifi(const std::string& interface, std::vector<WifiObservation>& out, std::string& error) {
  out.clear();
  ensureInterfaceUp(interface);
  std::string raw;
  if (!runCapture({"iwlist", interface, "scanning"}, raw, error)) {
    return false;
  }

  WifiObservation current;
  bool has_current = false;

  std::size_t pos = 0;
  while (pos <= raw.size()) {
    std::size_t newline = raw.find('\n', pos);
    std::string line = trim(raw.substr(pos, newline == std::string::npos ? std::string::npos
                                                                            : newline - pos));
    if (newline == std::string::npos) pos = raw.size() + 1;
    else pos = newline + 1;

    std::size_t cell_pos = line.find("Cell ");
    std::size_t addr_pos = line.find("Address: ");
    if (cell_pos != std::string::npos && addr_pos != std::string::npos) {
      flushObservation(current, has_current, out);
      has_current = true;
      current.bssid = trim(line.substr(addr_pos + std::strlen("Address: ")));
      continue;
    }
    if (!has_current) continue;

    std::size_t channel_pos = line.find("Channel:");
    if (channel_pos != std::string::npos) {
      current.channel = std::atoi(line.c_str() + channel_pos + std::strlen("Channel:"));
      continue;
    }

    std::size_t frequency_pos = line.find("Frequency:");
    if (frequency_pos != std::string::npos) {
      const double ghz = std::strtod(line.c_str() + frequency_pos + std::strlen("Frequency:"), nullptr);
      current.frequency_mhz = static_cast<int>(ghz * 1000.0 + 0.5);
      continue;
    }

    std::size_t signal_pos = line.find("Signal level=");
    if (signal_pos != std::string::npos) {
      current.rssi_dbm = std::atoi(line.c_str() + signal_pos + std::strlen("Signal level="));
      current.quality_percent = std::max(0, std::min(100, 2 * (current.rssi_dbm + 100)));
      continue;
    }

    std::size_t enc_pos = line.find("Encryption key:");
    if (enc_pos != std::string::npos) {
      std::string value = trim(line.substr(enc_pos + std::strlen("Encryption key:")));
      current.secured = (value == "on");
      current.security = current.secured ? "encrypted" : "open";
      continue;
    }

    if (line.find("WPA2") != std::string::npos || line.find("IEEE 802.11i") != std::string::npos) {
      current.security = "WPA2";
      continue;
    }
    if (line.find("WPA Version 1") != std::string::npos) {
      if (current.security != "WPA2") current.security = "WPA";
      continue;
    }

    std::size_t essid_pos = line.find("ESSID:\"");
    if (essid_pos != std::string::npos) {
      std::size_t start = essid_pos + std::strlen("ESSID:\"");
      std::size_t end = line.find('"', start);
      current.ssid = end == std::string::npos ? line.substr(start) : line.substr(start, end - start);
      continue;
    }
  }
  flushObservation(current, has_current, out);
  return true;
}

WifiSurveySummary analyseWifi(const std::vector<WifiObservation>& observations) {
  WifiSurveySummary summary;
  summary.total = static_cast<int>(observations.size());
  int loads[3] = {0, 0, 0};
  constexpr int channels[3] = {1, 6, 11};
  for (const auto& ap : observations) {
    if (!ap.secured) ++summary.open;
    if (ap.ssid.empty()) ++summary.hidden;
    if (ap.channel >= 1 && ap.channel <= 14) ++summary.two_ghz;
    else if (ap.channel > 14) ++summary.five_ghz;
    summary.strongest_rssi_dbm = std::max(summary.strongest_rssi_dbm, ap.rssi_dbm);
    for (std::size_t i = 0; i < 3; ++i) {
      const int distance = std::abs(ap.channel - channels[i]);
      if (distance <= 4) loads[i] += std::max(1, 100 + ap.rssi_dbm) * (5 - distance);
    }
  }
  summary.channel_load_1 = loads[0];
  summary.channel_load_6 = loads[1];
  summary.channel_load_11 = loads[2];
  summary.best_24_channel = channels[static_cast<std::size_t>(std::min_element(loads, loads + 3) - loads)];
  return summary;
}

}  // namespace reconclave
