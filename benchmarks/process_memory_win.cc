#include "benchmarks/process_memory.h"

#include <windows.h>  // IWYU pragma: keep

#include <processthreadsapi.h>
#include <psapi.h>

#include <cstdint>
#include <expected>
#include <string>

namespace vela::benchmarks {

std::expected<std::uint64_t, std::string> CurrentResidentBytes() {
  PROCESS_MEMORY_COUNTERS counters{};
  counters.cb = sizeof(counters);
  if (K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) == 0) {
    return std::unexpected("cannot read benchmark process working set");
  }
  return counters.WorkingSetSize;
}

std::expected<std::uint64_t, std::string> PeakResidentBytes() {
  PROCESS_MEMORY_COUNTERS counters{};
  counters.cb = sizeof(counters);
  if (K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) == 0) {
    return std::unexpected("cannot read benchmark process peak working set");
  }
  return counters.PeakWorkingSetSize;
}

}  // namespace vela::benchmarks
