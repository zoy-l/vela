#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/native_program_mode.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/ir/ir.h"
#include "compiler/jit/code_cache_status.h"
#include "compiler/jit/code_resources.h"
#include "compiler/jit/code_reuse.h"
#include "compiler/native/compile_options.h"
#include "compiler/native/escape_analysis.h"
#include "compiler/native/native_roots.h"
#include "runtime/native/native_image.h"

namespace vela::compiler {
class SourceSession;
class DevelopmentFiles;
class DevelopmentSession;
struct SourceSessionConfiguration;
struct CompiledSourceSnapshot;
}

namespace vela::compiler::jit {

frontend::Diagnostics CompilationDiagnostic(std::string message);

inline constexpr std::string_view kCodeCacheOption = "--code-cache=";

enum class Materialization : std::uint8_t { Eager, OnDemand };

struct CompileOptions final {
  NativeOptimization optimization = NativeOptimization::Release;
  native::NativeDebugging debugging = native::NativeDebugging::Safepoints;
  NativeCodeTier tier = NativeCodeTier::Baseline;
  std::optional<std::filesystem::path> code_cache = std::nullopt;
  bool preserve_ir = false;
  native::EscapeOptimization escape = native::EscapeOptimization::Enabled;
  Materialization materialization = Materialization::Eager;
};

struct CompileTimings final {
  std::chrono::nanoseconds generation{};
  std::chrono::nanoseconds cache{};
  std::chrono::nanoseconds optimization{};
  std::chrono::nanoseconds compilation{};
  std::chrono::nanoseconds linking{};
};

struct CompiledImage final {
  // 别名所有者同时保活 ORC 资源、重定位、描述表及整个代码批次。
  std::shared_ptr<const runtime::NativeImage> image;
  std::string llvm_ir;
  std::vector<native::NativeFunctionMetrics> function_metrics;
  CodeCacheStatus cache = CodeCacheStatus::Disabled;
  CompileTimings timings;
  std::size_t compiled_functions = 0;
};

// 编译不访问 Isolate；未发布结果可以丢弃。映像最后引用在 Isolate 线程释放时退休 ORC
// 资源，且可晚于 Compiler 本身；AOT runtime 无需链接这些 LLVM 类型。
struct CompilerProcessOptions final {
  std::filesystem::path executable;
  std::filesystem::path cache;
};

// 公共 API：嵌入宿主通过此接口选择本地或进程外编译及源码文件会话。
class Compiler {
 public:
  [[nodiscard]] static std::expected<std::unique_ptr<Compiler>, frontend::Diagnostics> Create();
  // 公共 API：创建独立进程编译器，宿主保留运行时与代码加载。
  [[nodiscard]] static std::expected<std::unique_ptr<Compiler>, frontend::Diagnostics>
  CreateExternal(CompilerProcessOptions options);
  Compiler() = default;
  Compiler(const Compiler&) = delete;
  Compiler& operator=(const Compiler&) = delete;
  Compiler(Compiler&&) = delete;
  Compiler& operator=(Compiler&&) = delete;
  virtual ~Compiler() = default;
  virtual std::expected<std::shared_ptr<SourceSession>, frontend::Diagnostics> CreateSources(
      const SourceSessionConfiguration& configuration) = 0;
  // 公共 API：按编译器归属创建文件会话，外部编译器将监视与捕获留在编译进程。
  [[nodiscard]] virtual std::expected<std::unique_ptr<DevelopmentFiles>, frontend::Diagnostics>
  CreateFiles(const std::string& entry, const frontend::ModulePaths& imports) = 0;
  // program 来自 IR preparation；reuse 仅用于拥有版本身份的 Reloadable 程序。
  // 本地引用入口只支持同步发射；OnDemand 必须使用下面的共享所有权重载。
  [[nodiscard]] virtual std::expected<CompiledImage, frontend::Diagnostics> Compile(
      const ir::Program& program,
      const CompileOptions& options = {},
      CodeReuseCache* reuse = nullptr) = 0;
  // 按需发射持有源程序的不可变所有权；源码会话可借助 aliasing shared_ptr 共享当前快照。
  [[nodiscard]] virtual std::expected<CompiledImage, frontend::Diagnostics> Compile(
      std::shared_ptr<const ir::Program> program,
      const CompileOptions& options,
      CodeReuseCache* reuse = nullptr) = 0;
  [[nodiscard]] virtual CodeResourceCounts resource_counts() const noexcept = 0;

 protected:
  friend class ::vela::compiler::DevelopmentSession;
  virtual std::expected<CompiledImage, frontend::Diagnostics> CompileSource(
      const std::shared_ptr<const CompiledSourceSnapshot>& source,
      const CompileOptions& options,
      CodeReuseCache* reuse);
};

}  // namespace vela::compiler::jit
