#include "benchmarks/source.h"
#include <optional>
#include <string>
#include <string_view>

#include "benchmark/benchmark.h"

#include <utility>
#include "base/file.h"
#include "base/path.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/jit/compiler.h"
#include "compiler/jit/source_compiler.h"
#include "runtime/native/native_instance.h"

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

std::unique_ptr<runtime::NativeInstance> CompileBenchmarkSource(benchmark::State& state,
                                                                std::string_view source,
                                                                runtime::Isolate& isolate) {
  auto compiler = compiler::jit::Compiler::Create();
  if (!compiler) {
    state.SkipWithError(frontend::DiagnosticsText(compiler.error()));
    return nullptr;
  }
  auto compiled = compiler::jit::CompileSource(**compiler, source);
  if (!compiled.Succeeded()) {
    state.SkipWithError(frontend::DiagnosticsText(compiled.diagnostics));
    return nullptr;
  }
  auto instance = runtime::NativeInstance::Instantiate(isolate, std::move(compiled.code.image));
  if (!instance) {
    state.SkipWithError(instance.error().message);
    return nullptr;
  }
  const auto entry = (*instance)->FindFunction("main");
  if (!entry || !(*instance)->call_signature(*entry).parameters.empty()) {
    state.SkipWithError("Benchmark requires a zero-argument main function");
    return nullptr;
  }
  return std::move(*instance);
}

}  // namespace vela::benchmarks
#include <memory>
