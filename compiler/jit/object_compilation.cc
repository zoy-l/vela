#include "compiler/jit/object_compilation.h"

#include <chrono>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <llvm/IR/Module.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>

#include "compiler/jit/code_cache.h"
#include "compiler/jit/code_cache_status.h"
#include "compiler/jit/compiler.h"
#include "compiler/native/code_generator.h"

namespace vela::compiler::jit {

std::expected<CachedObject, std::string> CompileCachedObject(
    const std::optional<std::filesystem::path>& directory,
    std::string_view key,
    const std::function<std::expected<std::unique_ptr<llvm::MemoryBuffer>, std::string>()>& compile,
    CompileTimings* timings) {
  using Clock = std::chrono::steady_clock;
  CachedObject result{.status = CodeCacheStatus::Disabled, .object = {}};
  if (directory) {
    const auto started = timings != nullptr ? Clock::now() : Clock::time_point{};
    auto loaded = ReadCodeCache(*directory, key);
    if (timings != nullptr) {
      timings->cache += Clock::now() - started;
    }
    if (!loaded) {
      return std::unexpected(std::move(loaded.error()));
    }
    result = std::move(*loaded);
    if (result.status == CodeCacheStatus::Hit) {
      return result;
    }
  }
  auto compiled = compile();
  if (!compiled) {
    return std::unexpected(std::move(compiled.error()));
  }
  result.object = std::move(*compiled);
  if (directory) {
    const auto started = timings != nullptr ? Clock::now() : Clock::time_point{};
    auto saved = WriteCodeCache(*directory, key, result.object->getMemBufferRef());
    if (timings != nullptr) {
      timings->cache += Clock::now() - started;
    }
    if (!saved) {
      return std::unexpected(std::move(saved.error()));
    }
  }
  return result;
}

std::expected<CompiledObject, std::string> PrepareObject(native::NativeModule& generated,
                                                         const CompileOptions& options,
                                                         CompileTimings& timings) {
  using Clock = std::chrono::steady_clock;
  CompiledObject result{
      .cached = {.status = CodeCacheStatus::Disabled, .object = {}},
      .llvm_ir = {},
  };
  std::string key;
  if (options.code_cache) {
    const auto started = Clock::now();
    key = CodeCacheKey(*generated.module, options.optimization);
    timings.cache += Clock::now() - started;
  }
  const auto finalize = [&] -> std::expected<void, std::string> {
    const auto started = Clock::now();
    const auto finalized = native::Finalize(generated);
    timings.optimization = Clock::now() - started;
    if (!finalized) {
      return std::unexpected(finalized.error());
    }
    if (options.preserve_ir) {
      llvm::raw_string_ostream output(result.llvm_ir);
      generated.module->print(output, nullptr);
    }
    return {};
  };
  auto cached = CompileCachedObject(
      options.code_cache, key,
      [&] -> std::expected<std::unique_ptr<llvm::MemoryBuffer>, std::string> {
        if (const auto finalized = finalize(); !finalized) {
          return std::unexpected(finalized.error());
        }
        const auto started = Clock::now();
        auto object = native::EmitObject(generated);
        timings.compilation = Clock::now() - started;
        return object;
      },
      &timings);
  if (!cached) {
    return std::unexpected(std::move(cached.error()));
  }
  if (cached->status == CodeCacheStatus::Hit && options.preserve_ir) {
    if (const auto finalized = finalize(); !finalized) {
      return std::unexpected(finalized.error());
    }
  }
  result.cached = std::move(*cached);
  return result;
}

}  // namespace vela::compiler::jit
