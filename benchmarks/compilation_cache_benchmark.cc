#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "benchmark/benchmark.h"

#include "base/path.h"
#include "benchmarks/temporary_directory.h"
#include "compiler/bytecode/compilation_cache.h"
#include "compiler/bytecode/compiler.h"
#include "runtime/isolate/isolate.h"
#include "vm/bytecode_image.h"
#include "vm/program.h"

namespace vela::benchmarks {
namespace {

constexpr std::string_view kSmallSource = "string_interpolation.vela";

std::optional<vm::Program> CompileCacheInput(benchmark::State& state, runtime::Isolate& isolate) {
  const auto source = std::filesystem::path(VELA_BENCHMARK_SOURCE_DIR) / kSmallSource;
  auto compiled = compiler::CompileFile(PathText(source), isolate);
  if (!compiled.Succeeded()) {
    state.SkipWithError("cannot compile cache benchmark input");
    return std::nullopt;
  }
  return std::move(compiled.program);
}

std::optional<std::vector<std::byte>> Image(benchmark::State& state) {
  runtime::Isolate isolate;
  const auto program = CompileCacheInput(state, isolate);
  if (!program) {
    return std::nullopt;
  }
  const auto image = vm::SaveBytecode(*program, isolate);
  if (!image) {
    state.SkipWithError(image.error().message);
    return std::nullopt;
  }
  return *image;
}

void BytecodeLoad(benchmark::State& state) {
  const auto image = Image(state);
  if (!image) {
    return;
  }
  for (auto iteration : state) {
    runtime::Isolate isolate;
    auto loaded = vm::LoadBytecode(*image, isolate);
    if (!loaded) {
      state.SkipWithError(loaded.error().message);
      break;
    }
    benchmark::DoNotOptimize(loaded);
  }
  state.counters["image_bytes"] = static_cast<double>(image->size());
}

void BytecodeSave(benchmark::State& state) {
  runtime::Isolate isolate;
  const auto program = CompileCacheInput(state, isolate);
  if (!program) {
    return;
  }
  for (auto iteration : state) {
    auto image = vm::SaveBytecode(*program, isolate);
    if (!image) {
      state.SkipWithError(image.error().message);
      break;
    }
    benchmark::DoNotOptimize(image);
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
  {
    runtime::Isolate isolate;
    const auto initial = compiler::CompileFileCached(path, directory, isolate);
    if (!initial || !initial->compiled.Succeeded()) {
      state.SkipWithError("cannot populate cache");
      return;
    }
  }
  for (auto iteration : state) {
    runtime::Isolate isolate;
    auto loaded = compiler::CompileFileCached(path, directory, isolate);
    if (!loaded || loaded->status != compiler::CacheStatus::Hit) {
      state.SkipWithError("expected compilation cache hit");
      break;
    }
    benchmark::DoNotOptimize(loaded);
  }
}

void FileCompile(benchmark::State& state, std::string_view filename) {
  const auto source = std::filesystem::path(VELA_BENCHMARK_SOURCE_DIR) / filename;
  const auto path = PathText(source);
  for (auto iteration : state) {
    runtime::Isolate isolate;
    auto compiled = compiler::CompileFile(path, isolate);
    if (!compiled.Succeeded()) {
      state.SkipWithError("cannot compile input");
      break;
    }
    benchmark::DoNotOptimize(compiled);
  }
}

BENCHMARK(BytecodeLoad)->Name("cache/bytecode_load");
BENCHMARK(BytecodeSave)->Name("cache/bytecode_save");
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
