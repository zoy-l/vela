#include "benchmarks/process_memory.h"

#include <libproc.h>
#include <sys/proc_info.h>
#include <unistd.h>

#include <cstdint>
#include <expected>
#include <string>

namespace vela::benchmarks {

std::expected<std::uint64_t, std::string> CurrentResidentBytes() {
  proc_taskinfo information{};
  constexpr auto kRecordBytes = static_cast<int>(sizeof(information));
  if (proc_pidinfo(getpid(), PROC_PIDTASKINFO, 0, &information, kRecordBytes) != kRecordBytes) {
    return std::unexpected("cannot read benchmark process resident size");
  }
  return information.pti_resident_size;
}

}  // namespace vela::benchmarks
