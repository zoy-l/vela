#include "benchmarks/process_memory.h"

#include <sys/resource.h>
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

std::expected<std::uint64_t, std::string> PeakResidentBytes() {
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0) {
    return std::unexpected("cannot read benchmark process peak resident size");
  }
  constexpr std::uint64_t kBytesPerKibibyte = 1024;
  return static_cast<std::uint64_t>(usage.ru_maxrss) * kBytesPerKibibyte;
}

}  // namespace vela::benchmarks
