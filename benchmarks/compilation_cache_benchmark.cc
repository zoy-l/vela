#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "benchmark/benchmark.h"

#include "base/path.h"
#include "compiler/compilation_cache.h"
#include "compiler/compiler.h"
#include "runtime/isolate.h"
#include "vm/bytecode_image.h"

namespace vela::benchmarks {
namespace {

auto Image(benchmark::State& state) -> std::optional<std::vector<std::byte>> {
  runtime::Isolate isolate;
  const auto source =
      std::filesystem::path(VELA_BENCHMARK_SOURCE_DIR) / "string_interpolation.vela";
  const auto compiled = compiler::CompileFile(PathText(source), isolate);
  if (!compiled.Succeeded()) {
    state.SkipWithError("cannot compile cache benchmark input");
    return std::nullopt;
  }
  const auto image = vm::SaveBytecode(compiled.program, isolate);
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
  const auto source =
      std::filesystem::path(VELA_BENCHMARK_SOURCE_DIR) / "string_interpolation.vela";
  const auto compiled = compiler::CompileFile(PathText(source), isolate);
  if (!compiled.Succeeded()) {
    state.SkipWithError("cannot compile input");
    return;
  }
  for (auto iteration : state) {
    auto image = vm::SaveBytecode(compiled.program, isolate);
    if (!image) {
      state.SkipWithError(image.error().message);
      break;
    }
    benchmark::DoNotOptimize(image);
  }
}

void ContentCacheHit(benchmark::State& state, std::string_view filename) {
  std::error_code error;
  const auto temporary = std::filesystem::temp_directory_path(error);
  if (error) {
    state.SkipWithError(error.message());
    return;
  }
  const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto directory = temporary / ("vela-cache-benchmark-" + std::to_string(suffix));
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
  std::filesystem::remove_all(directory, error);
  if (error) {
    state.SkipWithError(error.message());
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
BENCHMARK_CAPTURE(ContentCacheHit, Small, "string_interpolation.vela")->Name("cache/content_hit");
BENCHMARK_CAPTURE(FileCompile, Small, "string_interpolation.vela")
    ->Name("cache/uncached_file_compile");
BENCHMARK_CAPTURE(ContentCacheHit, Unicode, "unicode_graphemes.vela")
    ->Name("cache/unicode_content_hit");
BENCHMARK_CAPTURE(FileCompile, Unicode, "unicode_graphemes.vela")
    ->Name("cache/unicode_file_compile");
BENCHMARK_CAPTURE(ContentCacheHit, Library, "library_linking.vela")
    ->Name("cache/library_content_hit");
BENCHMARK_CAPTURE(FileCompile, Library, "library_linking.vela")->Name("cache/library_file_compile");

}  // namespace
}  // namespace vela::benchmarks
