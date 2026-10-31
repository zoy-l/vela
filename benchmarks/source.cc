#include "benchmarks/source.h"
#include <optional>
#include <string>
#include <string_view>

#include "benchmark/benchmark.h"

#include "base/file.h"
#include "base/path.h"
#include "compiler/compiler.h"
#include "compiler/frontend/diagnostic.h"
#include "vm/program.h"

namespace vela::benchmarks {

std::optional<std::string> LoadBenchmarkSource(benchmark::State& state, std::string_view filename) {
  const auto path = Utf8Path(VELA_BENCHMARK_SOURCE_DIR) / Utf8Path(filename);
  const auto bytes = ReadBinaryFile(path);
  if (!bytes) {
    state.SkipWithError(bytes.error());
    return std::nullopt;
  }
  const void* storage = bytes->data();
  return std::string(static_cast<const char*>(storage), bytes->size());
}

std::optional<vm::FunctionHandle> CompileBenchmarkEntry(benchmark::State& state,
                                                        std::string_view source,
                                                        runtime::Isolate& isolate) {
  const auto compiled = compiler::CompileSource(source, isolate);
  if (!compiled.Succeeded()) {
    state.SkipWithError(frontend::DiagnosticsText(compiled.diagnostics));
    return std::nullopt;
  }
  auto entry = compiled.program.FindFunction("main");
  if (!entry.IsValid() || entry.parameter_count() != 0) {
    state.SkipWithError("Benchmark requires a zero-argument main function");
    return std::nullopt;
  }
  return entry;
}

}  // namespace vela::benchmarks
