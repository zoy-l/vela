#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "base/native_program_mode.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/ir/ir.h"
#include "compiler/native/array_analysis.h"
#include "compiler/native/borrow_analysis.h"
#include "compiler/native/call_targets.h"
#include "compiler/native/compile_options.h"
#include "compiler/native/escape_analysis.h"
#include "compiler/native/field_access.h"
#include "compiler/native/function_symbols.h"
#include "compiler/native/native_roots.h"
#include "compiler/native/target_version_emitter.h"

namespace llvm {
class LLVMContext;
class MemoryBuffer;
class Module;
class TargetMachine;
class Function;
}  // namespace llvm

namespace vela::compiler::native {

class NativeImageData;

void InitializeTarget();
[[nodiscard]] std::expected<std::string, std::string> TargetDataLayout();

struct CpuProfile final {
  std::string name;
  std::string features;
};

// 动态代码使用宿主已启用的指令集；离线产物保留通用 CPU 目标。
[[nodiscard]] CpuProfile TargetCpu(NativeCodePlacement placement);

// context 最后释放；AOT 目标文件与 ORC 交付共用同一份完整 LLVM 语义 lowering。
struct NativeModule final {
  NativeModule(NativeProgramMode mode,
               NativeCodePlacement placement,
               NativeOptimization optimization);
  ~NativeModule();
  NativeModule(NativeModule&&) noexcept;
  NativeModule& operator=(NativeModule&&) = delete;
  NativeModule(const NativeModule&) = delete;
  NativeModule& operator=(const NativeModule&) = delete;
  NativeProgramMode mode;
  NativeCodePlacement placement;
  NativeOptimization optimization;
  std::unique_ptr<llvm::LLVMContext> context;
  std::shared_ptr<llvm::TargetMachine> machine;
  std::unique_ptr<llvm::Module> module;
  std::string image_symbol;
  std::shared_ptr<NativeImageData> image_data;
  std::vector<llvm::Function*> entry_functions;
  std::vector<llvm::Function*> reflection_functions;
  std::vector<llvm::Function*> typed_functions;
  std::vector<llvm::Function*> integer_callbacks;
  std::vector<llvm::Function*> recursive_functions;
  std::vector<NativeTargetGroup> target_functions;
  std::vector<NativeFunctionMetrics> function_metrics;
  std::vector<std::vector<std::string>> function_symbols;
  std::vector<std::vector<std::size_t>> body_dependencies;
};

// 按需编译只保留规范 IR 的分析结果；每次发射独占临时 LLVM context 和 module。
struct BaselineCompilationPlan final {
  BaselineCompilationPlan(NativeDebugging debugging,
                          EscapeOptimization escape,
                          NativeOptimization optimization)
      : debugging(debugging), escape(escape), optimization(optimization) {}
  CpuProfile cpu;
  std::string data_layout;
  NativeDebugging debugging;
  EscapeOptimization escape;
  NativeOptimization optimization;
  NativeCallPlan calls;
  FieldTargets fields;
  ProgramArrayAccessFacts arrays;
  std::vector<std::optional<BorrowedFunction>> borrows;
  std::vector<FunctionEntrySymbols> symbols;
  std::vector<std::vector<std::size_t>> needed;
};

[[nodiscard]] std::expected<BaselineCompilationPlan, frontend::Diagnostics> PlanBaselineCompilation(
    const ir::Program& program,
    NativeDebugging debugging,
    EscapeOptimization escape,
    NativeOptimization optimization);
[[nodiscard]] std::expected<NativeModule, frontend::Diagnostics> GenerateBaselineFunction(
    const ir::Program& program,
    const BaselineCompilationPlan& plan,
    std::size_t function);

[[nodiscard]] std::expected<NativeModule, frontend::Diagnostics> Generate(
    const ir::Program& program,
    const NativeCallPlan& calls,
    const FieldTargets& field_targets,
    const std::string& module_name,
    NativeDebugging debugging = NativeDebugging::Safepoints,
    NativeCodeTier tier = NativeCodeTier::Baseline,
    EscapeOptimization escape = EscapeOptimization::Enabled,
    NativeCodePlacement placement = NativeCodePlacement::Linked,
    NativeOptimization optimization = NativeOptimization::Release,
    std::span<const std::uint8_t> reused = {});

// 调用者可在优化前添加 AOT 的公开包装和 main；JIT 直接交付映像入口。
[[nodiscard]] std::expected<void, std::string> Finalize(NativeModule& generated);
[[nodiscard]] std::expected<std::unique_ptr<llvm::MemoryBuffer>, std::string> EmitObject(
    NativeModule& generated);

}  // namespace vela::compiler::native
