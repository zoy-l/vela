#include "benchmarks/process_memory.h"

#include <unistd.h>

#include <cstdint>
#include <expected>
#include <fstream>
#include <string>

namespace vela::benchmarks {

std::expected<std::uint64_t, std::string> CurrentResidentBytes() {
  std::ifstream input("/proc/self/statm");
  std::uint64_t total = 0;
  std::uint64_t resident = 0;
  const auto page_bytes = sysconf(_SC_PAGESIZE);
  if (!(input >> total >> resident) || page_bytes <= 0) {
    return std::unexpected("cannot read benchmark process resident pages");
  }
  return resident * static_cast<std::uint64_t>(page_bytes);
}

}  // namespace vela::benchmarks
