#include "network_inventory.h"

#include <fstream>
#include <sstream>
#include <utility>

namespace reconclave {

bool readArpSnapshot(std::vector<ArpNeighbour>& out, std::string& error,
                     const std::string& path) {
  out.clear();
  std::ifstream input(path);
  if (!input) {
    error = "unable to read neighbour cache";
    return false;
  }

  std::string line;
  std::getline(input, line);  // Header.
  while (std::getline(input, line)) {
    std::istringstream row(line);
    std::string hardware_type, flags, mask;
    ArpNeighbour neighbour;
    if (!(row >> neighbour.address >> hardware_type >> flags >> neighbour.mac >> mask >> neighbour.interface)) {
      continue;
    }
    unsigned parsed_flags = 0;
    std::istringstream(flags) >> std::hex >> parsed_flags;
    neighbour.complete = (parsed_flags & 0x2U) != 0 && neighbour.mac != "00:00:00:00:00:00";
    out.push_back(std::move(neighbour));
  }
  error.clear();
  return true;
}

}  // namespace reconclave
