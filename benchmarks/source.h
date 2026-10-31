#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "vm/program.h"

namespace benchmark {
class State;
}
namespace vela::runtime {
class Isolate;
}

namespace vela::benchmarks {

std::optional<std::string> LoadBenchmarkSource(benchmark::State& state, std::string_view filename);
std::optional<vm::FunctionHandle> CompileBenchmarkEntry(benchmark::State& state,
                                                        std::string_view source,
                                                        runtime::Isolate& isolate);

}  // namespace vela::benchmarks
