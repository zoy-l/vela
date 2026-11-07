#include "compiler/native/code_generator.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SmallVectorMemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/Transforms/Scalar/LoopPassManager.h>
#include <llvm/Transforms/Scalar/SimpleLoopUnswitch.h>
#include <llvm/Transforms/Utils/PromoteMemToReg.h>

#include "base/builtin.h"
#include "base/native_program_mode.h"
#include "base/opcode.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"
#include "compiler/native/array_analysis.h"
#include "compiler/native/borrow_analysis.h"
#include "compiler/native/call_targets.h"
#include "compiler/native/escape_analysis.h"
#include "compiler/native/field_access.h"
#include "compiler/native/function_emitter.h"
#include "compiler/native/integer_bit_optimizer.h"
#include "compiler/native/integer_loop_optimizer.h"
#include "compiler/native/native_image_data.h"
#include "compiler/native/native_image_emitter.h"
#include "compiler/native/native_roots.h"
#include "compiler/native/native_values.h"
#include "compiler/native/recursive_inliner.h"
#include "runtime/native/native_image.h"

namespace vela::compiler::native {
namespace {

constexpr std::size_t kMaximumInlineInstructions = 64;

bool CanInlineSmallFunction(const ir::Function& function) {
  // 以源操作限制展开规模；安全点、溢出失败和恢复分支不应让几个标量操作失去内联。
  if (function.is_async || !function.handlers.empty() ||
      function.parameter_count > kMaximumInlineInstructions ||
      function.instructions.size() > kMaximumInlineInstructions) {
    return false;
  }
  return std::ranges::all_of(function.instructions, [](const ir::Instruction& instruction) {
    return instruction.opcode == Opcode::Return ||
           !ir::AnalyzeEffects(instruction).MayCollectOnSuccess();
  });
}

bool CanUnrollRecursion(const ir::Function& function, std::size_t function_index) {
  constexpr std::size_t kMaximumRecursiveCalls = 2;
  if (function.is_async || !function.handlers.empty() ||
      function.parameter_count > kMaximumInlineInstructions ||
      function.instructions.size() > kMaximumInlineInstructions ||
      !IsScalar(function.return_type) || !std::ranges::all_of(function.value_types, IsScalar)) {
    return false;
  }
  std::size_t calls = 0;
  for (const auto& instruction : function.instructions) {
    if (instruction.opcode == Opcode::Call && instruction.index == function_index) {
      ++calls;
    } else if (instruction.opcode != Opcode::Return &&
               ir::AnalyzeEffects(instruction).MayCollectOnSuccess()) {
      return false;
    }
  }
  return calls != 0 && calls <= kMaximumRecursiveCalls;
}

bool CanUseRecursiveLeaf(const ir::Function& function, std::size_t function_index) {
  if (!CanUnrollRecursion(function, function_index)) {
    return false;
  }
  for (std::size_t index = 0; index < function.instructions.size(); ++index) {
    const auto& instruction = function.instructions.at(index);
    if (instruction.opcode == Opcode::Return ||
        (instruction.opcode == Opcode::Call && instruction.index == function_index)) {
      continue;
    }
    if (instruction.opcode == Opcode::Jump || instruction.opcode == Opcode::JumpIfFalse ||
        instruction.opcode == Opcode::JumpIfNull) {
      if (instruction.index <= index) {
        return false;
      }
      continue;
    }
    const auto effects = ir::AnalyzeEffects(instruction);
    if (effects.reads_state || effects.writes_state || effects.may_suspend ||
        effects.changes_control || effects.MayCollectOnSuccess()) {
      return false;
    }
  }
  return true;
}

bool CanUnrollManagedRecursion(const ir::Function& function,
                               std::size_t function_index,
                               const NativeFunctionMetrics& metrics) {
  // 仅复制完整入口，不省略根、错误位置或逻辑深度。限制单个自调用和帧大小，
  // 由已有 LLVM 指令预算约束展开后的代码体积。
  constexpr std::size_t kMaximumExpandedFrameBytes = kMaximumNativeStackBytes / 8;
  return !function.is_async && function.handlers.empty() &&
         function.instructions.size() <= kMaximumInlineInstructions &&
         function.register_count <= kMaximumInlineInstructions &&
         metrics.frame_bytes <= kMaximumExpandedFrameBytes &&
         std::ranges::count_if(function.instructions, [&](const auto& instruction) {
           return instruction.opcode == Opcode::Call && instruction.index == function_index;
         }) == 1;
}

bool CanExpandRecursion(const ir::Program& program,
                        std::size_t index,
                        const NativeFunctionMetrics& metrics,
                        NativeCodePlacement placement,
                        NativeCodeTier tier) {
  if (program.native_mode != NativeProgramMode::Closed && tier != NativeCodeTier::Optimized) {
    return false;
  }
  const auto& function = program.functions[index];
  return CanUnrollRecursion(function, index) ||
         (placement == NativeCodePlacement::Linked &&
          CanUnrollManagedRecursion(function, index, metrics));
}

bool CanUseBoundedLeaf(const ir::Function& function) {
  if (!function.native_symbol.empty() || !IsScalar(function.return_type)) {
    return false;
  }
  for (std::size_t index = 0; index < function.instructions.size(); ++index) {
    const auto& instruction = function.instructions.at(index);
    if (instruction.opcode == Opcode::Return) {
      continue;
    }
    const auto effects = ir::AnalyzeEffects(instruction);
    const auto access = ir::AnalyzeRegisters(instruction, {});
    bool scalar = !access.defines_value || IsScalar(function.value_types.at(instruction.dst));
    ir::ForEachUse(instruction, access, [&](ir::RegisterId reg) {
      scalar = scalar && IsScalar(function.value_types.at(reg));
    });
    if (!scalar || effects.reads_state || effects.writes_state ||
        ((instruction.opcode == Opcode::Jump || instruction.opcode == Opcode::JumpIfFalse ||
          instruction.opcode == Opcode::JumpIfNull) &&
         instruction.index <= index)) {
      return false;
    }
  }
  return true;
}

bool IsReadonlyKernelInstruction(const ir::Instruction& instruction, const ir::Function& function) {
  if (instruction.opcode == Opcode::Return) {
    return true;
  }
  const auto effects = ir::AnalyzeEffects(instruction);
  const auto array_length = instruction.opcode == Opcode::BuiltinCall &&
                            instruction.index == static_cast<std::uint32_t>(Builtin::ArrayLength);
  if (effects.MayCollectOnSuccess() || effects.writes_state ||
      (effects.reads_state && instruction.opcode != Opcode::GetIndex && !array_length)) {
    return false;
  }
  const auto access = ir::AnalyzeRegisters(instruction, {});
  bool valid = true;
  ir::ForEachUse(instruction, access, [&](ir::RegisterId reg) {
    const auto& type = function.value_types.at(reg);
    valid = valid && (IsScalar(type) || instruction.opcode == Opcode::Move ||
                      (reg == instruction.lhs && type.kind == Kind::Array &&
                       (instruction.opcode == Opcode::GetIndex || array_length)));
  });
  return valid;
}

bool CanUseGuardedLeaf(const ir::Function& function) {
  constexpr std::size_t kMaximumKernelInstructions = 128;
  if (!function.native_symbol.empty() || function.is_async || !function.handlers.empty() ||
      !IsScalar(function.return_type) || function.parameter_count > kMaximumKernelInstructions ||
      function.instructions.size() > kMaximumKernelInstructions) {
    return false;
  }
  return std::ranges::all_of(function.instructions, [&](const auto& instruction) {
    return IsReadonlyKernelInstruction(instruction, function);
  });
}

bool HasVersionAssumptions(const ir::Function& function, const ir::Program& program) {
  return std::ranges::any_of(function.instructions, [&](const auto& instruction) {
    switch (instruction.opcode) {
      case Opcode::NewInstance:
      case Opcode::CallIndirect:
      case Opcode::CallInterface:
        return true;
      case Opcode::GetField:
      case Opcode::SetField:
        return instruction.field_declaration.has_value();
      case Opcode::Call:
        return program.functions.at(instruction.index).native_symbol.empty();
      default:
        return false;
    }
  });
}

std::expected<std::unique_ptr<llvm::TargetMachine>, std::string> CreateTarget(
    NativeCodePlacement placement) {
  InitializeTarget();
  // 编译器、运行时和产物必须使用同一个配置目标。LLVM 的默认 triple 可能带有
  // 与当前 SDK 不同的 Darwin 版本，直接读取它会让生成镜像无法在同一进程实例化。
  const auto triple = std::string(runtime::NativeTargetIdentity());
  std::string error;
  const auto* target = llvm::TargetRegistry::lookupTarget(triple, error);
  if (target == nullptr) {
    return std::unexpected(std::move(error));
  }
  llvm::TargetOptions options;
  const llvm::Triple target_triple(triple);
  if (placement == NativeCodePlacement::Linked && target_triple.getArch() == llvm::Triple::x86_64) {
    // x64 指令缓存行为 64 字节；固定热循环对齐，避免前序函数体积变化增加取指行数。
    constexpr unsigned kX64LoopAlignment = 64;
    options.LoopAlignment = kX64LoopAlignment;
  }
  const auto cpu = TargetCpu(placement);
  std::unique_ptr<llvm::TargetMachine> machine(target->createTargetMachine(
      target_triple, cpu.name, cpu.features, options, llvm::Reloc::PIC_, std::nullopt,
      llvm::CodeGenOptLevel::Default, placement == NativeCodePlacement::Dynamic));
  if (machine == nullptr) {
    return std::unexpected("unable to create native target machine");
  }
  return machine;
}

void Optimize(llvm::Module& module, llvm::TargetMachine& machine) {
  machine.setOptLevel(llvm::CodeGenOptLevel::Aggressive);
  llvm::LoopAnalysisManager loops;
  llvm::FunctionAnalysisManager functions;
  llvm::CGSCCAnalysisManager calls;
  llvm::ModuleAnalysisManager modules;
  llvm::PipelineTuningOptions tuning;
  // LLVM 库接口默认关闭 SLP；独立字段计算可安全并行，浮点仍沿用严格 IEEE 语义。
  tuning.SLPVectorization = true;
  llvm::PassBuilder passes(&machine, tuning);
  passes.registerPeepholeEPCallback(
      [](llvm::FunctionPassManager& pipeline, llvm::OptimizationLevel /*unused*/) {
        AddIntegerBitPasses(pipeline);
      });
  passes.registerCGSCCOptimizerLateEPCallback(
      [](llvm::CGSCCPassManager& pipeline, llvm::OptimizationLevel /*unused*/) {
        // 内联后、循环旋转前处理纯调用；旋转会把计数条件化简为退出相等判断。
        llvm::FunctionPassManager functions;
        AddBoundedPollPasses(functions);
        pipeline.addPass(llvm::createCGSCCToFunctionPassAdaptor(std::move(functions)));
      });
  // 宿主调用约定随实例固定；允许克隆循环，将 direct/checked 分派移到循环外。
  // 使用 LLVM 自带的体积阈值控制复制，不改变循环内错误和逻辑栈帧语义。
  passes.registerLateLoopOptimizationsEPCallback(
      [](llvm::LoopPassManager& pipeline, llvm::OptimizationLevel /*unused*/) {
        pipeline.addPass(llvm::SimpleLoopUnswitchPass(true));
      });
  passes.registerScalarOptimizerLateEPCallback(
      [](llvm::FunctionPassManager& pipeline, llvm::OptimizationLevel /*unused*/) {
        AddIntegerLoopPasses(pipeline);
      });
  passes.registerModuleAnalyses(modules);
  passes.registerCGSCCAnalyses(calls);
  passes.registerFunctionAnalyses(functions);
  passes.registerLoopAnalyses(loops);
  passes.crossRegisterProxies(loops, functions, calls, modules);
  llvm::FunctionPassManager integer_loops;
  AddIntegerLoopPasses(integer_loops);
  llvm::ModulePassManager preparation;
  preparation.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(integer_loops)));
  preparation.run(module, modules);
  auto pipeline = passes.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O3);
  pipeline.run(module, modules);
}

frontend::Diagnostics MakeDiagnostics(const ir::Program& program,
                                      std::string message,
                                      const ir::Function* function) {
  frontend::Diagnostic result{.location = {}, .message = std::move(message), .source_path = {}};
  if (function != nullptr) {
    result.location = function->location;
    if (!program.source_paths.empty()) {
      result.source_path = program.source_paths.at(result.location.source_id);
    }
  }
  return {std::move(result)};
}

std::expected<llvm::FunctionType*, std::string> BodySignature(llvm::Module& module,
                                                              const ir::Function& function) {
  auto& context = module.getContext();
  std::vector<llvm::Type*> parameters{llvm::PointerType::getUnqual(context)};
  std::uint64_t argument_bytes = 0;
  for (std::size_t parameter = 0; parameter < function.parameter_count; ++parameter) {
    parameters.push_back(ValueType(context, function.value_types.at(parameter)));
    argument_bytes += module.getDataLayout().getTypeAllocSize(parameters.back());
  }
  if (argument_bytes > kMaximumNativeStackBytes) {
    return std::unexpected("native typed arguments exceed the native call stack budget");
  }
  if (!function.native_symbol.empty()) {
    parameters.push_back(NativeImportType(context));
  }
  parameters.push_back(llvm::PointerType::getUnqual(context));
  return llvm::FunctionType::get(llvm::Type::getInt8Ty(context), parameters, false);
}

GeneratedFunction AddRecoveryEntries(llvm::Module& module,
                                     const std::string& prefix,
                                     GeneratedFunction& baseline) {
  auto& context = module.getContext();
  auto* pointer = llvm::PointerType::getUnqual(context);
  auto* recovery_type =
      llvm::FunctionType::get(llvm::Type::getInt8Ty(context), {pointer, pointer, pointer}, false);
  const auto create = [&](llvm::FunctionType* type, const std::string& suffix) {
    return llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage, prefix + suffix,
                                  module);
  };
  baseline.recovery = create(recovery_type, "_recover");
  auto* resume = baseline.resume;
  if (resume != nullptr) {
    baseline.resume_recovery = create(recovery_type, "_recover_resume");
  }
  auto optimized = baseline;
  optimized.baseline_body = baseline.body;
  optimized.baseline_resume = resume;
  optimized.body = create(baseline.body->getFunctionType(), "_optimized");
  if (resume != nullptr) {
    optimized.resume = create(resume->getFunctionType(), "_optimized_resume");
  }
  return optimized;
}

llvm::Function* AddResumeEntry(llvm::Module& module,
                               const ir::Function& function,
                               const std::string& symbol) {
  if (!function.is_async ||
      !std::ranges::any_of(function.instructions, [](const auto& instruction) {
        return instruction.opcode == Opcode::Await;
      })) {
    return nullptr;
  }
  auto& context = module.getContext();
  auto* pointer = llvm::PointerType::getUnqual(context);
  auto* type = llvm::FunctionType::get(llvm::Type::getInt8Ty(context), {pointer, pointer}, false);
  return llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage, symbol, module);
}

struct FunctionDeclarations final {
  std::vector<GeneratedFunction> baseline;
  std::vector<GeneratedFunction> optimized;
  std::vector<std::optional<ir::ClassId>> borrowable;
};

void SetTypedArguments(std::span<llvm::Function* const> entries, const ir::Function& source) {
  for (auto* entry : entries) {
    if (entry == nullptr) {
      continue;
    }
    for (std::size_t parameter = 0; parameter < source.parameter_count; ++parameter) {
      const auto& type = source.value_types.at(parameter);
      if (IsReferenceKind(type.kind) && !type.nullable) {
        // 非空契约已在装箱入口和受查转换处验证；内部调用保留此事实供内联与 GVN 使用。
        entry->addParamAttr(static_cast<unsigned>(parameter + 1), llvm::Attribute::NonNull);
      }
    }
  }
}

enum class LeafKind : std::uint8_t { None, Bounded, Guarded, Recursive };

struct InliningPlan final {
  bool enabled;
  LeafKind leaf;
};

InliningPlan PlanInlining(const ir::Function& function,
                          std::size_t function_index,
                          NativeProgramMode mode,
                          NativeDebugging debugging,
                          NativeCodePlacement placement) {
  const auto small = CanInlineSmallFunction(function);
  const auto guarded = CanUseGuardedLeaf(function);
  auto leaf = LeafKind::None;
  if (function.native_symbol.empty() && debugging == NativeDebugging::Safepoints &&
      mode == NativeProgramMode::Closed) {
    if (small && CanUseBoundedLeaf(function)) {
      leaf = LeafKind::Bounded;
    } else if (guarded) {
      leaf = LeafKind::Guarded;
    } else if (placement == NativeCodePlacement::Linked &&
               CanUseRecursiveLeaf(function, function_index)) {
      leaf = LeafKind::Recursive;
    }
  }
  return {.enabled = small || guarded, .leaf = leaf};
}

void AddLeafEntry(llvm::Module& module,
                  const std::string& prefix,
                  LeafKind kind,
                  GeneratedFunction& function) {
  if (kind == LeafKind::None) {
    return;
  }
  // 直接调用已确认当前版本；守卫限制不轮询的工作量，失败交还调用者建立逻辑帧。
  auto* type = function.body->getFunctionType();
  if (kind == LeafKind::Recursive) {
    std::vector<llvm::Type*> parameters(type->param_begin(), type->param_end());
    parameters.insert(parameters.end() - 1, llvm::Type::getInt32Ty(module.getContext()));
    type = llvm::FunctionType::get(type->getReturnType(), parameters, false);
    function.budgeted_recursion = true;
  }
  function.leaf =
      llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage, prefix + "_leaf", module);
  function.leaf->addFnAttr(llvm::Attribute::AlwaysInline);
  if (kind == LeafKind::Guarded) {
    auto* guard_type =
        llvm::FunctionType::get(llvm::Type::getInt1Ty(module.getContext()), type->params(), false);
    function.leaf_guard = llvm::Function::Create(guard_type, llvm::GlobalValue::InternalLinkage,
                                                 prefix + "_leaf_guard", module);
    function.leaf_guard->addFnAttr(llvm::Attribute::AlwaysInline);
  }
}

std::vector<std::uint8_t> NeededFunctions(const ir::Program& program,
                                          NativeCodeTier tier,
                                          std::span<const std::uint8_t> reused) {
  const auto count = program.functions.size();
  std::vector<std::uint8_t> needed(count, reused.empty() ? 1 : 0);
  for (std::size_t index = 0; index < reused.size(); ++index) {
    if (reused[index] != 0) {
      continue;
    }
    needed.at(index) = 1;
    const auto& function = program.functions.at(index);
    for (const auto& instruction : function.instructions) {
      if (instruction.opcode == Opcode::Call) {
        needed.at(instruction.index) = 1;
      }
    }
    if (tier == NativeCodeTier::Optimized) {
      for (const auto& target : FindDirectCalls(function, program)) {
        if (target) {
          needed.at(target->function) = 1;
        }
      }
    }
  }
  return needed;
}

std::expected<FunctionDeclarations, frontend::Diagnostics> DeclareFunctions(
    llvm::Module& module,
    const ir::Program& program,
    const std::string& module_name,
    NativeCodeTier tier,
    NativeDebugging debugging,
    NativeCodePlacement placement,
    std::span<const std::optional<BorrowedFunction>> borrows,
    std::span<const std::uint8_t> needed) {
  FunctionDeclarations declarations;
  auto& generated = declarations.baseline;
  auto& optimized = declarations.optimized;
  auto& borrowable = declarations.borrowable;
  const auto count = program.functions.size();
  generated.reserve(count);
  optimized.reserve(count);
  borrowable.resize(count);
  auto& context = module.getContext();
  for (std::size_t index = 0; index < count; ++index) {
    if (needed[index] == 0) {
      generated.emplace_back();
      optimized.emplace_back();
      continue;
    }
    const auto& function = program.functions.at(index);
    const auto body_type = BodySignature(module, function);
    if (!body_type) {
      return std::unexpected(MakeDiagnostics(program, body_type.error(), &function));
    }
    auto* body = llvm::Function::Create(*body_type, llvm::GlobalValue::InternalLinkage,
                                        module_name + "_body_" + std::to_string(index), module);
    auto* pointer = llvm::PointerType::getUnqual(context);
    auto* wrapper_type = llvm::FunctionType::get(
        llvm::Type::getInt8Ty(context),
        {pointer, pointer, llvm::Type::getInt64Ty(context), pointer}, false);
    const auto symbol = module_name + "_entry_" + std::to_string(index);
    auto* entry =
        llvm::Function::Create(wrapper_type, llvm::GlobalValue::InternalLinkage, symbol, module);
    auto* resume =
        AddResumeEntry(module, function, module_name + "_resume_" + std::to_string(index));
    GeneratedFunction baseline{
        .body = body,
        .entry = entry,
        .resume = resume,
        .square_root = IsSquareRootImport(function, program.layouts),
    };
    auto current = baseline;
    if (program.native_mode == NativeProgramMode::Reloadable && tier == NativeCodeTier::Optimized &&
        function.native_symbol.empty()) {
      // 仅可重载映像存在版本失效；封闭映像的调用目标和字段布局在实例期内固定。
      if (HasVersionAssumptions(function, program)) {
        current = AddRecoveryEntries(module, module_name + '_' + std::to_string(index), baseline);
      }
    }
    const auto inlining = PlanInlining(function, index, program.native_mode, debugging, placement);
    if (inlining.leaf != LeafKind::None) {
      AddLeafEntry(module, module_name + '_' + std::to_string(index), inlining.leaf, current);
      baseline.leaf = current.leaf;
      baseline.leaf_guard = current.leaf_guard;
      baseline.budgeted_recursion = current.budgeted_recursion;
    }
    if (inlining.enabled) {
      body->addFnAttr(llvm::Attribute::AlwaysInline);
      current.body->addFnAttr(llvm::Attribute::AlwaysInline);
    }
    if (const auto& borrow = borrows[index]) {
      baseline.borrowed =
          llvm::Function::Create(*body_type, llvm::GlobalValue::InternalLinkage,
                                 module_name + "_borrowed_" + std::to_string(index), module);
      borrowable.at(index) = borrow->type;
      current.borrowed = baseline.borrowed;
    }
    const std::array typed_entries{body, current.body, current.leaf, baseline.borrowed};
    SetTypedArguments(typed_entries, function);
    generated.push_back(baseline);
    optimized.push_back(current);
  }
  return declarations;
}

std::vector<std::string> FunctionSymbols(const GeneratedFunction& baseline,
                                         const GeneratedFunction& optimized) {
  std::vector<std::string> symbols;
  const std::array functions{
      baseline.body,        baseline.entry,           baseline.resume,
      baseline.recovery,    baseline.resume_recovery, baseline.borrowed,
      optimized.body,       optimized.resume,         optimized.leaf,
      optimized.leaf_guard,
  };
  for (const auto* function : functions) {
    if (function == nullptr) {
      continue;
    }
    const auto name = function->getName().str();
    if (std::ranges::find(symbols, name) == symbols.end()) {
      symbols.push_back(name);
    }
  }
  return symbols;
}

std::expected<NativeFunctionMetrics, std::string> EmitDeclaredFunction(
    llvm::Module& module,
    const ir::Program& program,
    const NativeCallPlan& calls,
    const FieldTargets& field_targets,
    std::span<const std::uint64_t> array_element_bits,
    llvm::Constant* image,
    std::size_t index,
    const FunctionDeclarations& declarations,
    std::vector<std::size_t>& body_dependencies,
    NativeDebugging debugging,
    NativeCodeTier tier,
    EscapeOptimization escape,
    NativeCodePlacement placement,
    std::span<const std::optional<BorrowedFunction>> borrows) {
  const auto& generated = declarations.baseline;
  const auto& optimized = declarations.optimized;
  const auto& function = program.functions.at(index);
  const auto& target = optimized.at(index);
  const auto emission_entries = tier == NativeCodeTier::Optimized && target.baseline_body == nullptr
                                    ? std::span<const GeneratedFunction>(optimized)
                                    : std::span<const GeneratedFunction>(generated);
  const auto emission_tier = target.baseline_body == nullptr ? tier : NativeCodeTier::Baseline;
  auto emitted = EmitFunction(module, program, calls, field_targets, array_element_bits, image,
                              index, emission_entries, body_dependencies, debugging, placement,
                              emission_tier, escape, declarations.borrowable);
  if (!emitted) {
    return emitted;
  }
  if (const auto& borrow = borrows[index]) {
    const auto borrowed =
        EmitBorrowedFunction(module, program, calls, field_targets, array_element_bits, image,
                             index, borrow->plan, generated, debugging, placement);
    if (!borrowed) {
      return std::unexpected(borrowed.error());
    }
    emitted->borrowed_frame_bytes = borrowed->frame_bytes;
    emitted->borrowed_shadow_slots = borrowed->shadow_slots;
    emitted->borrowed_root_stores = borrowed->root_stores;
  }
  if (target.baseline_body != nullptr) {
    auto optimized_metrics = EmitFunction(module, program, calls, field_targets, array_element_bits,
                                          image, index, optimized, body_dependencies, debugging,
                                          placement, NativeCodeTier::Optimized, escape);
    if (!optimized_metrics) {
      return optimized_metrics;
    }
    emitted = std::move(optimized_metrics);
  }
  EmitWrapper(module, function, static_cast<std::uint32_t>(index), target, image, placement);
  return emitted;
}

void BindDynamicEntries(NativeModule& output,
                        std::span<llvm::Function* const> entries,
                        std::span<const GeneratedFunction> functions) {
  output.entry_functions.assign(entries.begin(), entries.end());
  for (const auto& function : functions) {
    auto* typed = function.body;
    output.typed_functions.push_back(typed);
    if (typed != nullptr) {
      typed->setLinkage(llvm::GlobalValue::ExternalLinkage);
    }
  }
  for (auto* entry : entries) {
    if (entry != nullptr) {
      entry->setLinkage(llvm::GlobalValue::ExternalLinkage);
    }
  }
}

}  // namespace

void InitializeTarget() {
  static const bool initialized = [] {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    return true;
  }();
  static_cast<void>(initialized);
}

CpuProfile TargetCpu(NativeCodePlacement placement) {
  if (placement == NativeCodePlacement::Linked) {
    return {.name = "generic", .features = {}};
  }
  std::vector<std::string> features;
  for (const auto& feature : llvm::sys::getHostCPUFeatures()) {
    features.push_back(std::string(feature.getValue() ? "+" : "-") + feature.getKey().str());
  }
  std::ranges::sort(features);
  std::string joined;
  for (const auto& feature : features) {
    if (!joined.empty()) {
      joined += ',';
    }
    joined += feature;
  }
  return {.name = llvm::sys::getHostCPUName().str(), .features = std::move(joined)};
}

NativeModule::NativeModule() = default;
NativeModule::~NativeModule() = default;
NativeModule::NativeModule(NativeModule&&) noexcept = default;

std::expected<NativeModule, frontend::Diagnostics> Generate(const ir::Program& program,
                                                            const NativeCallPlan& calls,
                                                            const FieldTargets& field_targets,
                                                            const std::string& module_name,
                                                            NativeDebugging debugging,
                                                            NativeCodeTier tier,
                                                            EscapeOptimization escape,
                                                            NativeCodePlacement placement,
                                                            std::span<const std::uint8_t> reused) {
  if (debugging == NativeDebugging::Instructions) {
    // 逐指令检查需要真实对象与规范 IR 值，不让标量替换隐藏可观察的对象字段。
    escape = EscapeOptimization::Disabled;
  }
  const auto diagnostic = [&](std::string message, const ir::Function* function = nullptr) {
    return std::unexpected(MakeDiagnostics(program, std::move(message), function));
  };
  auto target = CreateTarget(placement);
  if (!target) {
    return diagnostic(target.error());
  }
  NativeModule output;
  output.context = std::make_unique<llvm::LLVMContext>();
  output.machine = std::move(*target);
  output.body_dependencies = calls.dependencies;
  output.module = std::make_unique<llvm::Module>(module_name, *output.context);
  auto& module = *output.module;
  module.setTargetTriple(output.machine->getTargetTriple());
  module.setDataLayout(output.machine->createDataLayout());
  module.setCodeModel(output.machine->getCodeModel());
  const auto borrows = escape == EscapeOptimization::Enabled
                           ? PlanBorrows(program)
                           : std::vector<std::optional<BorrowedFunction>>(program.functions.size());
  const auto declarations =
      DeclareFunctions(module, program, module_name, tier, debugging, placement, borrows,
                       NeededFunctions(program, tier, reused));
  if (!declarations) {
    return std::unexpected(declarations.error());
  }
  const auto& generated = declarations->baseline;
  const auto& optimized = declarations->optimized;
  std::vector<llvm::Function*> entries;
  entries.reserve(generated.size());
  for (const auto& function : generated) {
    entries.push_back(function.entry);
  }
  auto description = DescribeNativeImage(program);
  llvm::Constant* image =
      llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*output.context));
  if (placement == NativeCodePlacement::Dynamic &&
      program.native_mode == NativeProgramMode::Reloadable) {
    output.image_data = std::move(description);
    BindDynamicEntries(output, entries, optimized);
  } else {
    const auto emitted = EmitNativeImage(module, *description->image(), entries, module_name);
    if (!emitted) {
      return diagnostic(emitted.error());
    }
    output.image_symbol = emitted->symbol;
    image = emitted->data;
  }
  std::vector<NativeFunctionMetrics> metrics;
  // 在逃逸改写前分析一次规范程序，所有入口和代码层级共享同一组写入事实。
  const auto array_element_bits = FindArrayElementBits(program);
  for (std::size_t index = 0; index < program.functions.size(); ++index) {
    output.function_symbols.push_back(FunctionSymbols(generated.at(index), optimized.at(index)));
    if (!reused.empty() && reused[index] != 0) {
      metrics.emplace_back();
      continue;
    }
    const auto& function = program.functions.at(index);
    auto& dependencies = output.body_dependencies.at(index);
    auto emitted = EmitDeclaredFunction(module, program, calls, field_targets,
                                        array_element_bits.at(index), image, index, *declarations,
                                        dependencies, debugging, tier, escape, placement, borrows);
    if (!emitted) {
      return diagnostic(emitted.error(), &function);
    }
    metrics.push_back(*emitted);
    std::ranges::sort(dependencies);
    const auto duplicates = std::ranges::unique(dependencies);
    dependencies.erase(duplicates.begin(), duplicates.end());
    // 恢复布局由只读描述共享；优化 JIT 与封闭代码采用相同的有界展开预算，
    // 各逻辑帧仍保留独立的恢复位置和活跃值，不展开基线动态分派。
    if (CanExpandRecursion(program, index, *emitted, placement, tier)) {
      const auto& function_entries = optimized.at(index);
      output.recursive_functions.push_back(
          function_entries.budgeted_recursion ? function_entries.leaf : function_entries.body);
    }
  }
  output.function_metrics = std::move(metrics);
  for (auto& function : module) {
    if (!function.isDeclaration()) {
      // 标准 LLVM 目标属性同时参与机器码生成和对象缓存身份，避免跨 CPU 误用产物。
      function.addFnAttr("target-cpu", output.machine->getTargetCPU());
      function.addFnAttr("target-features", output.machine->getTargetFeatureString());
    }
  }
  return output;
}

std::expected<void, std::string> Finalize(NativeModule& generated,
                                          NativeOptimization optimization) {
  auto& module = *generated.module;
  std::string verification;
  llvm::raw_string_ostream verifier(verification);
  if (llvm::verifyModule(module, &verifier)) {
    return std::unexpected(verification);
  }
  if (optimization == NativeOptimization::Release) {
    for (auto* function : generated.recursive_functions) {
      InlineRecursion(*function);
    }
    generated.recursive_functions.clear();
    Optimize(module, *generated.machine);
  } else {
    // 局部 SSA 是帧存储契约的一部分。即使关闭优化，也不能把大量从不借用地址的
    // 临时值全部留在物理栈上，绕过大帧的有界外部存储。
    for (auto& function : module) {
      if (function.isDeclaration()) {
        continue;
      }
      llvm::SmallVector<llvm::AllocaInst*, 0> locals;
      for (auto& instruction : function.getEntryBlock()) {
        auto* local = llvm::dyn_cast<llvm::AllocaInst>(&instruction);
        if (local != nullptr && llvm::isAllocaPromotable(local)) {
          locals.push_back(local);
        }
      }
      llvm::DominatorTree dominators(function);
      llvm::PromoteMemToReg(locals, dominators);
    }
  }
  if (llvm::verifyModule(module, &verifier)) {
    return std::unexpected(verification);
  }
  return {};
}

std::expected<std::unique_ptr<llvm::MemoryBuffer>, std::string> EmitObject(
    NativeModule& generated) {
  llvm::SmallVector<char, 0> bytes;
  llvm::raw_svector_ostream stream(bytes);
  llvm::legacy::PassManager codegen;
  if (generated.machine->addPassesToEmitFile(codegen, stream, nullptr,
                                             llvm::CodeGenFileType::ObjectFile)) {
    return std::unexpected("native target cannot emit object files");
  }
  codegen.run(*generated.module);
  return std::make_unique<llvm::SmallVectorMemoryBuffer>(std::move(bytes),
                                                         generated.module->getName(), false);
}

}  // namespace vela::compiler::native
