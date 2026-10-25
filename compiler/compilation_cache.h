#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

#include "compiler/compiler.h"
#include "runtime/isolate.h"

namespace vela::compiler {

enum class CacheStatus : std::uint8_t { Hit, Absent, Stale, Invalid };

struct CachedCompileResult final {
  CompileResult compiled;
  CacheStatus status = CacheStatus::Absent;
};

// 命中时仍验证全部输入内容与路径身份；产物只属于传入的 isolate。
// 缓存损坏可重建；缓存目录或写入失败返回 I/O 错误，不冒充成功缓存。
[[nodiscard]] std::expected<CachedCompileResult, std::string> CompileFileCached(
    std::string_view path,
    const std::filesystem::path& cache_directory,
    runtime::Isolate& isolate);

}  // namespace vela::compiler
