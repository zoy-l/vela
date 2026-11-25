#pragma once

#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <llvm/Support/MemoryBuffer.h>

#include "compiler/jit/code_cache.h"

namespace vela::compiler::native {
struct NativeModule;
}

namespace vela::compiler::jit {

struct CompileOptions;
struct CompileTimings;

[[nodiscard]] std::expected<CachedObject, std::string> CompileCachedObject(
    const std::optional<std::filesystem::path>& directory,
    std::string_view key,
    const std::function<std::expected<std::unique_ptr<llvm::MemoryBuffer>, std::string>()>& compile,
    CompileTimings* timings = nullptr);

struct CompiledObject final {
  CachedObject cached;
  std::string llvm_ir;
};

[[nodiscard]] std::expected<CompiledObject, std::string> PrepareObject(
    native::NativeModule& generated,
    const CompileOptions& options,
    CompileTimings& timings);

}  // namespace vela::compiler::jit
