#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

#include <llvm/ExecutionEngine/Orc/LLJIT.h>

#include "compiler/frontend/diagnostic.h"
#include "compiler/ir/ir.h"
#include "compiler/native/code_generator.h"
#include "compiler/native/native_roots.h"
#include "runtime/native_image.h"

namespace vela::compiler::jit {

struct CompileOptions final {
  NativeOptimization optimization = NativeOptimization::Release;
  bool preserve_ir = false;
};

struct CompiledImage final {
  // 别名所有者同时保活 ORC 资源、重定位、描述表及整个代码批次。
  std::shared_ptr<const runtime::NativeImage> image;
  std::string llvm_ir;
  std::vector<native::NativeFunctionMetrics> function_metrics;
};

struct CodeResourceCounts final {
  std::size_t batches = 0;
  std::size_t functions = 0;
};

struct CodeResourceCounters final {
  std::atomic_size_t batches{0};
  std::atomic_size_t functions{0};
};

// 编译不访问 Isolate；未发布结果可以丢弃。映像最后引用在 Isolate 线程释放时退休 ORC
// 资源，且可晚于 Compiler 本身；AOT runtime 无需链接这些 LLVM 类型。
class Compiler final {
 public:
  [[nodiscard]] static std::expected<std::unique_ptr<Compiler>, frontend::Diagnostics> Create();
  [[nodiscard]] std::expected<CompiledImage, frontend::Diagnostics> Compile(
      const ir::Program& program,
      const CompileOptions& options = {});
  [[nodiscard]] CodeResourceCounts resource_counts() const noexcept;

 private:
  explicit Compiler(std::shared_ptr<llvm::orc::LLJIT> jit);
  std::shared_ptr<llvm::orc::LLJIT> jit_;
  std::shared_ptr<CodeResourceCounters> counters_;
  std::atomic_uint64_t next_module_{0};
};

}  // namespace vela::compiler::jit
