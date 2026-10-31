#include <filesystem>
#include <string>
#include <string_view>

#include "benchmark/benchmark.h"

#include "base/path.h"
#include "benchmarks/temporary_directory.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/jit/code_cache.h"
#include "compiler/jit/compiler.h"
#include "compiler/jit/source_compiler.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_instance.h"

namespace vela::benchmarks {
namespace {

constexpr std::string_view kSmallSource = "string_interpolation.vela";

void InstantiateImage(benchmark::State& state) {
  auto compiler = compiler::jit::Compiler::Create();
  if (!compiler) {
    state.SkipWithError(frontend::DiagnosticsText(compiler.error()));
    return;
  }
  const auto source = std::filesystem::path(VELA_BENCHMARK_SOURCE_DIR) / kSmallSource;
  const auto compiled = compiler::jit::CompileFile(**compiler, PathText(source));
  if (!compiled.Succeeded()) {
    state.SkipWithError(frontend::DiagnosticsText(compiled.diagnostics));
    return;
  }
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    runtime::Isolate isolate;
    auto instance = runtime::NativeInstance::Instantiate(isolate, compiled.code.image);
    if (!instance) {
      state.SkipWithError(instance.error().message);
      return;
    }
    benchmark::DoNotOptimize(instance);
  }
}

void ContentCacheHit(benchmark::State& state, std::string_view filename) {
  const BenchmarkDirectory temporary(state);
  if (!temporary.IsValid()) {
    return;
  }
  const auto& directory = temporary.path();
  const auto source = std::filesystem::path(VELA_BENCHMARK_SOURCE_DIR) / filename;
  const auto path = PathText(source);
  auto compiler = compiler::jit::Compiler::Create();
  if (!compiler) {
    state.SkipWithError(frontend::DiagnosticsText(compiler.error()));
    return;
  }
  {
    const auto initial = compiler::jit::CompileFile(
        **compiler, path, {.diagnostics = {}, .compilation = {.code_cache = directory}});
    if (!initial.Succeeded()) {
      state.SkipWithError("cannot populate cache");
      return;
    }
  }
  for (auto iteration : state) {
    auto loaded = compiler::jit::CompileFile(
        **compiler, path, {.diagnostics = {}, .compilation = {.code_cache = directory}});
    if (!loaded.Succeeded() || loaded.code.cache != compiler::jit::CodeCacheStatus::Hit) {
      state.SkipWithError("expected compilation cache hit");
      break;
    }
    benchmark::DoNotOptimize(loaded);
  }
}

void FileCompile(benchmark::State& state, std::string_view filename) {
  auto compiler = compiler::jit::Compiler::Create();
  if (!compiler) {
    state.SkipWithError(frontend::DiagnosticsText(compiler.error()));
    return;
  }
  const auto source = std::filesystem::path(VELA_BENCHMARK_SOURCE_DIR) / filename;
  const auto path = PathText(source);
  for (auto iteration : state) {
    auto compiled = compiler::jit::CompileFile(**compiler, path);
    if (!compiled.Succeeded()) {
      state.SkipWithError("cannot compile input");
      break;
    }
    benchmark::DoNotOptimize(compiled);
  }
}

BENCHMARK(InstantiateImage)->Name("cache/native_instantiate");
BENCHMARK_CAPTURE(ContentCacheHit, Small, kSmallSource)->Name("cache/content_hit");
BENCHMARK_CAPTURE(FileCompile, Small, kSmallSource)->Name("cache/uncached_file_compile");
BENCHMARK_CAPTURE(ContentCacheHit, Unicode, "unicode_graphemes.vela")
    ->Name("cache/unicode_content_hit");
BENCHMARK_CAPTURE(FileCompile, Unicode, "unicode_graphemes.vela")
    ->Name("cache/unicode_file_compile");
BENCHMARK_CAPTURE(ContentCacheHit, Library, "library_linking.vela")
    ->Name("cache/library_content_hit");
BENCHMARK_CAPTURE(FileCompile, Library, "library_linking.vela")->Name("cache/library_file_compile");

}  // namespace
}  // namespace vela::benchmarks
