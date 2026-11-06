#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/native_program_mode.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/ir/ir.h"
#include "compiler/native/call_targets.h"
#include "compiler/native/escape_analysis.h"
#include "compiler/native/field_access.h"
#include "compiler/native/native_roots.h"

namespace llvm {
class LLVMContext;
class MemoryBuffer;
class Module;
class TargetMachine;
class Function;
}  // namespace llvm

namespace vela::compiler {

enum class NativeOptimization : std::uint8_t { None, Release };
enum class NativeCodePlacement : std::uint8_t { Linked, Dynamic };

[[nodiscard]] constexpr std::string_view NativeOptimizationName(NativeOptimization optimization) {
  return optimization == NativeOptimization::Release ? "O3" : "O0";
}

namespace native {

class NativeImageData;

void InitializeTarget();

struct CpuProfile final {
  std::string name;
  std::string features;
};

// 动态代码使用宿主已启用的指令集；离线产物保留通用 CPU 目标。
[[nodiscard]] CpuProfile TargetCpu(NativeCodePlacement placement);

// context 最后释放；AOT 目标文件与 ORC 交付共用同一份完整 LLVM 语义 lowering。
struct NativeModule final {
  NativeModule();
  ~NativeModule();
  NativeModule(NativeModule&&) noexcept;
  NativeModule& operator=(NativeModule&&) = delete;
  NativeModule(const NativeModule&) = delete;
  NativeModule& operator=(const NativeModule&) = delete;
  std::unique_ptr<llvm::LLVMContext> context;
  std::unique_ptr<llvm::TargetMachine> machine;
  std::unique_ptr<llvm::Module> module;
  std::string image_symbol;
  std::shared_ptr<NativeImageData> image_data;
  std::vector<llvm::Function*> entry_functions;
  std::vector<llvm::Function*> typed_functions;
  std::vector<llvm::Function*> recursive_functions;
  std::vector<NativeFunctionMetrics> function_metrics;
  std::vector<std::vector<std::string>> function_symbols;
  std::vector<std::vector<std::size_t>> body_dependencies;
};

[[nodiscard]] std::expected<NativeModule, frontend::Diagnostics> Generate(
    const ir::Program& program,
    const NativeCallPlan& calls,
    const FieldTargets& field_targets,
    const std::string& module_name,
    NativeDebugging debugging = NativeDebugging::Safepoints,
    NativeCodeTier tier = NativeCodeTier::Baseline,
    EscapeOptimization escape = EscapeOptimization::Enabled,
    NativeCodePlacement placement = NativeCodePlacement::Linked,
    std::span<const std::uint8_t> reused = {});

// 调用者可在优化前添加 AOT 的公开包装和 main；JIT 直接交付映像入口。
[[nodiscard]] std::expected<void, std::string> Finalize(NativeModule& generated,
                                                        NativeOptimization optimization);
[[nodiscard]] std::expected<std::unique_ptr<llvm::MemoryBuffer>, std::string> EmitObject(
    NativeModule& generated);

}  // namespace native
}  // namespace vela::compiler
