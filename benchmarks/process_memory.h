#pragma once

#include <cstdint>
#include <expected>
#include <string>

namespace vela::benchmarks {

// 当前进程的 resident working set；与 Vela GC 堆计数分开报告。
[[nodiscard]] std::expected<std::uint64_t, std::string> CurrentResidentBytes();

}  // namespace vela::benchmarks
