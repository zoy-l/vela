#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "runtime/native/native_instance.h"

namespace benchmark {
class State;
}
namespace vela::runtime {
class Isolate;
}

namespace vela::benchmarks {

std::optional<std::string> LoadBenchmarkSource(benchmark::State& state, std::string_view filename);
std::unique_ptr<runtime::NativeInstance> CompileBenchmarkSource(benchmark::State& state,
                                                                std::string_view source,
                                                                runtime::Isolate& isolate);

}  // namespace vela::benchmarks
