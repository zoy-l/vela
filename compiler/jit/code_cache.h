#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/MemoryBufferRef.h>

#include "compiler/native/code_generator.h"

namespace llvm {
class Module;
}

namespace vela::compiler::jit {

enum class CodeCacheStatus : std::uint8_t { Disabled, Miss, Hit, Invalid };

struct CachedObject final {
  CodeCacheStatus status = CodeCacheStatus::Miss;
  std::unique_ptr<llvm::MemoryBuffer> object;
};

// 生成模块包含完整源码、声明、依赖和执行模式事实。缓存键还绑定编译器、LLVM、目标和
// runtime ABI；目标文件只保存重定位，不携带其他已装载代码批次或 Isolate 的地址。
[[nodiscard]] std::string CodeCacheKey(const llvm::Module& module, NativeOptimization optimization);
[[nodiscard]] std::expected<CachedObject, std::string> ReadCodeCache(
    const std::filesystem::path& directory,
    std::string_view key);
[[nodiscard]] std::expected<void, std::string> WriteCodeCache(
    const std::filesystem::path& directory,
    std::string_view key,
    llvm::MemoryBufferRef object);

}  // namespace vela::compiler::jit
