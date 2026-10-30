#pragma once

#include <cstdint>
#include <expected>
#include <string>

namespace vela::benchmarks {

// 当前进程的 resident working set；与 Vela GC 堆计数分开报告。
[[nodiscard]] std::expected<std::uint64_t, std::string> CurrentResidentBytes();
// 进程自启动以来的 resident 高水位；单独启动一个用例才能归因到该编译负载。
[[nodiscard]] std::expected<std::uint64_t, std::string> PeakResidentBytes();

}  // namespace vela::benchmarks
