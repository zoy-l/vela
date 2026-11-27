#include "compiler/native/code_generator.h"
#include "compiler/native/compile_options.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/ConstantRange.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Pass.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SmallVectorMemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/Transforms/IPO/AlwaysInliner.h>
#include <llvm/Transforms/Scalar/DCE.h>
#include <llvm/Transforms/Scalar/LoopPassManager.h>
#include <llvm/Transforms/Scalar/SimpleLoopUnswitch.h>

#include "base/callable.h"
#include "base/native_program_mode.h"
#include "base/opcode.h"
#include "base/reflection.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"
#include "compiler/native/array_analysis.h"
#include "compiler/native/array_reduction_optimizer.h"
#include "compiler/native/borrow_analysis.h"
#include "compiler/native/call_loop_optimizer.h"
#include "compiler/native/call_targets.h"
#include "compiler/native/escape_analysis.h"
#include "compiler/native/field_access.h"
#include "compiler/native/field_loop_optimizer.h"
#include "compiler/native/function_declarations.h"
#include "compiler/native/function_emitter.h"
#include "compiler/native/function_symbols.h"
#include "compiler/native/global_analysis.h"
#include "compiler/native/host_entry_emitter.h"
#include "compiler/native/integer_bit_optimizer.h"
#include "compiler/native/integer_loop_optimizer.h"
#include "compiler/native/invariant_call_optimizer.h"
#include "compiler/native/native_image_data.h"
#include "compiler/native/native_image_emitter.h"
#include "compiler/native/native_roots.h"
#include "compiler/native/native_values.h"
#include "compiler/native/recursive_inliner.h"
#include "compiler/native/repeated_field_optimizer.h"
#include "compiler/native/root_store_optimizer.h"
#include "compiler/native/target_version_emitter.h"
#include "runtime/native/host_callback.h"
#include "runtime/native/native_image.h"

namespace vela::compiler::native {
namespace {

constexpr std::size_t kMaximumRecursiveCalls = 2;

bool HasUnrollableRecursion(const ir::Function& function, std::size_t function_index) {
  if (!ir::CanAnalyzeSynchronousFunction(function) ||
      function.instructions.size() > kMaximumInlineInstructions) {
    return false;
  }
  const auto calls = std::ranges::count_if(function.instructions, [&](const auto& instruction) {
    return instruction.opcode == Opcode::Call && instruction.index == function_index;
  });
  return calls > 0 && std::cmp_less_equal(calls, kMaximumRecursiveCalls);
}

bool HasScalarRecursiveFrame(const ir::Function& function) {
  return function.parameter_count <= kMaximumInlineInstructions && IsScalar(function.return_type) &&
         std::ranges::all_of(function.value_types, IsScalar);
}

bool CanUnrollRecursion(const ir::Function& function, std::size_t function_index) {
  if (!HasScalarRecursiveFrame(function)) {
    return false;
  }
  return std::ranges::all_of(function.instructions, [&](const auto& instruction) {
    return instruction.opcode == Opcode::Return ||
           (instruction.opcode == Opcode::Call && instruction.index == function_index) ||
           !ir::AnalyzeEffects(function, instruction).MayCollectOnSuccess();
  });
}

bool CanUseRecursiveLeaf(const ir::Function& function, std::size_t function_index) {
  if (!HasUnrollableRecursion(function, function_index) || !HasScalarRecursiveFrame(function)) {
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
    const auto effects = ir::AnalyzeEffects(function, instruction);
    if (effects.MayCollectOnSuccess() || effects.reads_state || effects.writes_state ||
        effects.may_suspend || effects.changes_control) {
      return false;
    }
  }
  return true;
}

bool CanUnrollManagedRecursion(const ir::Function& function, const NativeFunctionMetrics& metrics) {
  // 仅复制完整入口，不省略根、错误位置或逻辑深度。此处限制寄存器数量和帧大小，
  // 递归形状由调用方验证，展开后的代码体积由已有 LLVM 指令预算约束。
  constexpr std::size_t kMaximumExpandedFrameBytes = kMaximumNativeStackBytes / 8;
  return function.register_count <= kMaximumInlineInstructions &&
         metrics.frame_bytes <= kMaximumExpandedFrameBytes;
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
  if (!HasUnrollableRecursion(function, index)) {
    return false;
  }
  return CanUnrollRecursion(function, index) ||
         (placement == NativeCodePlacement::Linked && CanUnrollManagedRecursion(function, metrics));
}

bool HasVersionAssumptions(const ir::Function& function, const ir::Program& program) {
  return std::ranges::any_of(function.instructions, [&](const auto& instruction) {
    switch (instruction.opcode) {
      case Opcode::NewInstance:
      case Opcode::CallIndirect:
      case Opcode::CallVirtual:
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
    NativeCodePlacement placement,
    NativeOptimization optimization,
    const CpuProfile& cpu) {
  InitializeTarget();
  // 编译器、运行时和产物必须使用同一个配置目标。LLVM 的默认 triple 可能带有
  // 与当前 SDK 不同的 Darwin 版本，直接读取它会让生成镜像无法在同一进程实例化。
  const llvm::Triple triple{std::string(runtime::NativeTargetIdentity())};
  std::string error;
  const auto* target = llvm::TargetRegistry::lookupTarget(triple, error);
  if (target == nullptr) {
    return std::unexpected(std::move(error));
  }
  llvm::TargetOptions options;
  if (placement == NativeCodePlacement::Linked && triple.getArch() == llvm::Triple::x86_64) {
    // x64 指令缓存行为 64 字节；固定热循环对齐，避免前序函数体积变化增加取指行数。
    constexpr unsigned kX64LoopAlignment = 64;
    options.LoopAlignment = kX64LoopAlignment;
  }
  if (placement == NativeCodePlacement::Linked && triple.isOSDarwin() && triple.isAArch64()) {
    // 部分展开的调用循环采用 16 字节对齐；仍使用通用指令集，不绑定宿主 CPU。
    constexpr unsigned kDarwinArm64LoopAlignment = 16;
    options.LoopAlignment = kDarwinArm64LoopAlignment;
  }
  // 基线保留局部 SSA 与轻量机器码整理，限制物理栈溢出槽。
  auto codegen_optimization = llvm::CodeGenOptLevel::Default;
  if (optimization == NativeOptimization::None) {
    codegen_optimization = llvm::CodeGenOptLevel::None;
  } else if (optimization == NativeOptimization::Baseline) {
    codegen_optimization = llvm::CodeGenOptLevel::Less;
  }
  std::unique_ptr<llvm::TargetMachine> machine(target->createTargetMachine(
      triple, cpu.name, cpu.features, options, llvm::Reloc::PIC_, std::nullopt,
      codegen_optimization, placement == NativeCodePlacement::Dynamic));
  if (machine == nullptr) {
    return std::unexpected("unable to create native target machine");
  }
  return machine;
}

std::expected<std::unique_ptr<llvm::TargetMachine>, std::string> CreateTarget(
    NativeCodePlacement placement,
    NativeOptimization optimization) {
  return CreateTarget(placement, optimization, TargetCpu(placement));
}

EscapeOptimization EffectiveEscape(NativeDebugging debugging, EscapeOptimization escape) {
  // 逐指令观察需要规范对象和 IR 值。
  return debugging == NativeDebugging::Instructions ? EscapeOptimization::Disabled : escape;
}

void Optimize(llvm::Module& module,
              llvm::TargetMachine& machine,
              NativeProgramMode mode,
              NativeCodePlacement placement) {
  machine.setOptLevel(llvm::CodeGenOptLevel::Aggressive);
  llvm::LoopAnalysisManager loops;
  llvm::FunctionAnalysisManager functions;
  llvm::CGSCCAnalysisManager calls;
  llvm::ModuleAnalysisManager modules;
  llvm::PipelineTuningOptions tuning;
  // LLVM 库接口默认关闭 SLP；独立字段计算可安全并行，浮点仍沿用严格 IEEE 语义。
  tuning.SLPVectorization = true;
  llvm::PassBuilder passes(&machine, tuning);
  const auto field_unrolling = IsClosedLinkedProgram(mode, placement)
                                   ? FieldLoopUnrolling::Enabled
                                   : FieldLoopUnrolling::Disabled;
  passes.registerPeepholeEPCallback(
      [mode, placement](llvm::FunctionPassManager& pipeline, llvm::OptimizationLevel /*unused*/) {
        AddIntegerBitPasses(pipeline);
        // 在通用循环展开前消费闭合字段事实，避免热路径被展开后失去重复遍历结构。
        if (IsClosedLinkedProgram(mode, placement)) {
          AddRepeatedFieldPasses(pipeline);
        }
      });
  passes.registerCGSCCOptimizerLateEPCallback(
      [field_unrolling](llvm::CGSCCPassManager& pipeline, llvm::OptimizationLevel /*unused*/) {
        // 内联后、循环旋转前处理纯调用；旋转会把计数条件化简为退出相等判断。
        llvm::FunctionPassManager functions;
        AddInvariantCallPasses(functions);
        AddBoundedPollPasses(functions, field_unrolling);
        pipeline.addPass(llvm::createCGSCCToFunctionPassAdaptor(std::move(functions)));
      });
  // 宿主调用约定随实例固定；允许克隆循环，将 direct/checked 分派移到循环外。
  // 使用 LLVM 自带的体积阈值控制复制，不改变循环内错误和逻辑栈帧语义。
  passes.registerLateLoopOptimizationsEPCallback(
      [](llvm::LoopPassManager& pipeline, llvm::OptimizationLevel /*unused*/) {
        pipeline.addPass(llvm::SimpleLoopUnswitchPass(true));
      });
  passes.registerScalarOptimizerLateEPCallback(
      [field_unrolling](llvm::FunctionPassManager& pipeline, llvm::OptimizationLevel /*unused*/) {
        AddIntegerLoopPasses(pipeline, field_unrolling);
        AddInvariantCallPasses(pipeline);
      });
  passes.registerVectorizerStartEPCallback(
      [mode, placement](llvm::FunctionPassManager& pipeline, llvm::OptimizationLevel /*unused*/) {
        AddLateBoundedPollPasses(pipeline);
        AddArrayReductionPasses(pipeline);
        if (IsClosedLinkedProgram(mode, placement)) {
          AddFieldLoopPasses(pipeline);
        }
      });
  passes.registerModuleAnalyses(modules);
  passes.registerCGSCCAnalyses(calls);
  passes.registerFunctionAnalyses(functions);
  passes.registerLoopAnalyses(loops);
  passes.crossRegisterProxies(loops, functions, calls, modules);
  llvm::FunctionPassManager integer_loops;
  AddIntegerLoopPasses(integer_loops, field_unrolling);
  AddInvariantCallPasses(integer_loops);
  AddBoundedPollPasses(integer_loops, field_unrolling);
  if (IsClosedLinkedProgram(mode, placement)) {
    AddRecursiveLoopHints(integer_loops);
  }
  llvm::ModulePassManager preparation;
  preparation.addPass(llvm::AlwaysInlinerPass());
  preparation.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(integer_loops)));
  preparation.run(module, modules);
  auto pipeline = passes.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O3);
  pipeline.run(module, modules);
  {
    llvm::FunctionPassManager call_loops;
    AddLateBoundedPollPasses(call_loops);
    AddArrayReductionPasses(call_loops);
    if (IsClosedLinkedProgram(mode, placement)) {
      AddCallLoopPasses(call_loops);
      AddLateFieldGuardPasses(call_loops);
    }
    AddRootStorePasses(call_loops);
    call_loops.addPass(llvm::DCEPass());
    llvm::ModulePassManager finishing;
    finishing.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(call_loops)));
    finishing.run(module, modules);
  }
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
    parameters.push_back(parameter == 0 && function.struct_receiver
                             ? llvm::PointerType::getUnqual(context)
                             : ValueType(context, function.value_types.at(parameter)));
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

bool HasResumeEntry(const ir::Function& function) {
  return function.is_async &&
         std::ranges::any_of(function.instructions, [](const auto& instruction) {
           return instruction.opcode == Opcode::Await;
         });
}

enum class FunctionRole : std::uint8_t { Prefix, Body, Entry, Reflection, Resume, Borrowed };
constexpr std::string_view kBaselineModuleName = "vela_jit";

std::string FunctionSymbol(const std::string& module, FunctionRole role, std::size_t index) {
  constexpr std::array<std::string_view, 6> kRoles{
      "_", "_body_", "_entry_", "_reflection_", "_resume_", "_borrowed_",
  };
  auto symbol = module;
  symbol.append(kRoles[static_cast<std::size_t>(role)]);
  symbol.append(std::to_string(index));
  return symbol;
}

std::vector<std::optional<BorrowedFunction>> FunctionBorrows(const ir::Program& program,
                                                             EscapeOptimization escape) {
  return escape == EscapeOptimization::Enabled
             ? PlanBorrows(program)
             : std::vector<std::optional<BorrowedFunction>>(program.functions.size());
}

void InitializeModule(NativeModule& output, const std::string& name) {
  output.context = std::make_unique<llvm::LLVMContext>();
  output.module = std::make_unique<llvm::Module>(name, *output.context);
  auto& module = *output.module;
  module.setTargetTriple(output.machine->getTargetTriple());
  module.setDataLayout(output.machine->createDataLayout());
  module.setCodeModel(output.machine->getCodeModel());
}

llvm::Function* AddResumeEntry(llvm::Module& module,
                               const ir::Function& function,
                               const std::string& symbol) {
  if (!HasResumeEntry(function)) {
    return nullptr;
  }
  auto& context = module.getContext();
  auto* pointer = llvm::PointerType::getUnqual(context);
  auto* type = llvm::FunctionType::get(llvm::Type::getInt8Ty(context), {pointer, pointer}, false);
  return llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage, symbol, module);
}

struct InliningPlan final {
  bool enabled;
  LeafKind leaf;
};

InliningPlan PlanInlining(const ir::Function& function,
                          std::size_t function_index,
                          NativeProgramMode mode,
                          NativeDebugging debugging,
                          NativeCodePlacement placement,
                          NativeCodeTier tier) {
  const auto small = CanInlineSmallFunction(function);
  auto leaf = LeafKind::None;
  if (mode == NativeProgramMode::Closed || tier == NativeCodeTier::Optimized) {
    leaf = PlanNonrecursiveLeaf(function, debugging);
    if (leaf == LeafKind::None && mode == NativeProgramMode::Closed &&
        placement == NativeCodePlacement::Linked &&
        CanUseScalarFieldBody(function, mode, debugging)) {
      leaf = LeafKind::Bounded;
    }
    if (leaf == LeafKind::None && debugging == NativeDebugging::Safepoints &&
        mode == NativeProgramMode::Closed && placement == NativeCodePlacement::Linked &&
        CanUseRecursiveLeaf(function, function_index)) {
      leaf = LeafKind::Recursive;
    }
  }
  // Guarded 入口的完整函数体保留安全点与恢复状态，只强制内联独立的叶入口。
  return {.enabled = small && leaf != LeafKind::Guarded, .leaf = leaf};
}

bool HasScalarCallbackSignature(const ir::Function& function) {
  const auto& signature = function.signature;
  const auto& result = function.return_type;
  if (!ir::CanAnalyzeSynchronousFunction(function) || !signature ||
      signature->parameters.size() != 1 || signature->hidden_parameters > 1 ||
      function.parameter_count != 1U + signature->hidden_parameters ||
      function.register_count > kMaximumInlineInstructions ||
      function.instructions.size() > kMaximumInlineInstructions || result.nullable ||
      (result.kind != Kind::Int && result.kind != Kind::Double && result.kind != Kind::Bool)) {
    return false;
  }
  const auto& parameter = signature->parameters.front();
  const auto& argument = function.value_types[signature->hidden_parameters];
  return !parameter.optional && !parameter.rest && !parameter.has_default &&
         argument.kind == result.kind && !argument.nullable;
}

bool IsIntegerCallback(const ir::Function& function) {
  if (function.return_type.kind != Kind::Int || !HasScalarCallbackSignature(function)) {
    return false;
  }
  // 只接受原有标量发射器处理的直线 IR。失败前不会写入脚本状态，可进入普通异常边界。
  return std::ranges::all_of(function.instructions, [](const auto& instruction) {
    if (instruction.construction || instruction.completes_construction || instruction.handler) {
      return false;
    }
    switch (instruction.opcode) {
      case Opcode::LoadInt:
      case Opcode::Move:
      case Opcode::AddInt:
      case Opcode::SubInt:
      case Opcode::MulInt:
      case Opcode::DivInt:
      case Opcode::ModInt:
      case Opcode::NegInt:
      case Opcode::BitNotInt:
      case Opcode::BitAndInt:
      case Opcode::BitOrInt:
      case Opcode::BitXorInt:
      case Opcode::ShiftLeftInt:
      case Opcode::ShiftRightInt:
      case Opcode::ShiftUnsignedRightInt:
      case Opcode::Return:
        return true;
      default:
        return false;
    }
  });
}

bool IsCaptureCell(const SourceType& type) {
  return type.kind == Kind::Class && type.name == ir::kCaptureCellName && !type.nullable;
}

std::optional<runtime::NativeInlineScalarOperand> ScalarCapture(
    std::span<const ir::Instruction> instructions,
    const ir::RegisterFlow& flow,
    const ir::Instruction& source) {
  if (flow.origins[source.lhs] == 0) {
    return runtime::NativeInlineScalarOperand{.capture = source.index};
  }
  const auto receiver = flow.OriginDefinition(source.lhs);
  if (!receiver || source.index != 0) {
    return std::nullopt;
  }
  const auto& binding = instructions[*receiver];
  if (binding.opcode != Opcode::GetField || flow.origins[binding.lhs] != 0) {
    return std::nullopt;
  }
  return runtime::NativeInlineScalarOperand{.capture = binding.index, .from_cell = true};
}

bool IsScalarCallbackCapture(const ir::Function& function, const ir::Instruction& instruction) {
  // Lambda 首参是捕获环境。仅借用直接保存的整数，或编译器创建且不会迁移的 Cell 字段。
  if (function.return_type.kind != Kind::Int || function.parameter_count != 2 ||
      instruction.field_declaration || function.value_types.front().kind != Kind::Function) {
    return false;
  }
  const auto& receiver = function.value_types[instruction.lhs];
  const auto& target = function.value_types[instruction.dst];
  const auto integer = target.kind == Kind::Int && !target.nullable;
  return (instruction.lhs == 0 && (integer || IsCaptureCell(target))) ||
         (IsCaptureCell(receiver) && instruction.index == 0 && integer);
}

struct ScalarCallbackBody final {
  const ir::Instruction* operation = nullptr;
};

std::optional<ScalarCallbackBody> AnalyzeScalarCallbackBody(const ir::Function& function) {
  const auto& instructions = function.instructions;
  if (instructions.empty() || instructions.back().opcode != Opcode::Return) {
    return std::nullopt;
  }
  const auto kind = function.return_type.kind;
  ScalarCallbackBody body;
  for (const auto& instruction : std::span(instructions).first(instructions.size() - 1)) {
    if (instruction.handler || instruction.construction || instruction.completes_construction) {
      return std::nullopt;
    }
    if (instruction.opcode == Opcode::GetField && IsScalarCallbackCapture(function, instruction)) {
      continue;
    }
    switch (instruction.opcode) {
      case Opcode::LoadInt:
      case Opcode::LoadDouble:
      case Opcode::LoadBool:
        break;
      case Opcode::Move: {
        const auto& source = function.value_types[instruction.lhs];
        if (source.kind != kind || source.nullable) {
          return std::nullopt;
        }
        break;
      }
      case Opcode::AddInt:
      case Opcode::SubInt:
      case Opcode::AddDouble:
      case Opcode::NotBool:
        if (body.operation != nullptr) {
          return std::nullopt;
        }
        body.operation = &instruction;
        break;
      default:
        return std::nullopt;
    }
    const auto& target = function.value_types.at(instruction.dst);
    if (target.kind != kind || target.nullable) {
      return std::nullopt;
    }
  }
  return body;
}

std::optional<runtime::NativeInlineScalarOperand> ResolveScalarCallbackOperand(
    const ir::Function& function,
    const ir::RegisterFlow& flow,
    const ir::Instruction& operation,
    ir::RegisterId parameter) {
  const auto left = flow.origins[operation.lhs];
  if (operation.opcode == Opcode::NotBool) {
    if (left == parameter) {
      return runtime::NativeInlineScalarOperand{.bits = 1};
    }
    return std::nullopt;
  }
  const auto right = flow.origins[operation.rhs];
  if (left != parameter && (operation.opcode != Opcode::AddInt || right != parameter)) {
    return std::nullopt;
  }
  const auto operand = flow.OriginDefinition(left == parameter ? operation.rhs : operation.lhs);
  if (!operand) {
    return std::nullopt;
  }
  const auto& instructions = function.instructions;
  const auto& source = instructions[*operand];
  if (source.opcode == Opcode::LoadDouble && operation.opcode == Opcode::AddDouble) {
    return runtime::NativeInlineScalarOperand{
        .bits = std::bit_cast<std::uint64_t>(function.doubles.at(source.index)),
    };
  }
  if (source.opcode == Opcode::GetField && operation.opcode == Opcode::AddInt) {
    return ScalarCapture(instructions, flow, source);
  }
  if (source.opcode != Opcode::LoadInt) {
    return std::nullopt;
  }
  auto addend = source.immediate;
  if (operation.opcode == Opcode::SubInt) {
    if (addend == std::numeric_limits<std::int64_t>::min()) {
      return std::nullopt;
    }
    addend = -addend;
  }
  return runtime::NativeInlineScalarOperand{.bits = std::bit_cast<std::uint64_t>(addend)};
}

std::optional<runtime::NativeInlineScalarOperand> ScalarCallbackOperand(
    const ir::Function& function) {
  if (!HasScalarCallbackSignature(function)) {
    return std::nullopt;
  }
  const auto body = AnalyzeScalarCallbackBody(function);
  if (!body) {
    return std::nullopt;
  }
  const auto accesses = ir::AnalyzeRegisterAccesses(function, {});
  const auto flow = ir::AnalyzeRegisterFlow(function, accesses);
  const auto output = flow.origins[function.instructions.back().dst];
  const auto parameter = static_cast<ir::RegisterId>(function.parameter_count - 1);
  const auto* operation = body->operation;
  if (operation == nullptr) {
    if (function.return_type.kind != Kind::Double && output == parameter) {
      return runtime::NativeInlineScalarOperand{};
    }
    return std::nullopt;
  }
  if (output != operation->dst) {
    return std::nullopt;
  }
  return ResolveScalarCallbackOperand(function, flow, *operation, parameter);
}

void AddLeafEntry(llvm::Module& module,
                  const std::string& prefix,
                  const ir::Function& source,
                  LeafKind kind,
                  GeneratedFunction& function) {
  if (kind == LeafKind::None) {
    return;
  }
  // 直接调用已确认当前版本；守卫限制不轮询的工作量，失败交还调用者建立逻辑帧。
  auto* type = function.body->getFunctionType();
  std::vector<llvm::Type*> parameters(type->param_begin(), type->param_end());
  const LeafEntryLayout layout(source.parameter_count);
  parameters.insert(parameters.begin() + layout.depth, llvm::Type::getInt64Ty(module.getContext()));
  if (kind == LeafKind::Recursive) {
    parameters.insert(parameters.begin() + layout.budget,
                      llvm::Type::getInt32Ty(module.getContext()));
    function.budgeted_recursion = true;
  }
  type = llvm::FunctionType::get(type->getReturnType(), parameters, false);
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

std::vector<std::size_t> NeededFunctions(const ir::Program& program,
                                         const NativeCallPlan& calls,
                                         std::span<const std::uint8_t> reused) {
  const auto count = program.functions.size();
  std::vector<std::uint8_t> needed(count, reused.empty() ? 1 : 0);
  for (std::size_t index = 0; index < reused.size(); ++index) {
    if (reused[index] != 0) {
      continue;
    }
    needed.at(index) = 1;
    const auto& function = program.functions.at(index);
    ForEachDirectCallee(function, calls.direct_calls[index],
                        [&](std::size_t callee) { needed.at(callee) = 1; });
  }
  std::vector<std::size_t> indices;
  for (std::size_t index = 0; index < count; ++index) {
    if (needed[index] != 0) {
      indices.push_back(index);
    }
  }
  return indices;
}

void AddCallbackLeafEntries(llvm::Module& module,
                            const std::string& prefix,
                            const ir::Function& function,
                            LeafKind kind,
                            GeneratedFunction& baseline,
                            GeneratedFunction& current) {
  if (kind == LeafKind::None) {
    return;
  }
  AddLeafEntry(module, prefix, function, kind, current);
  baseline.leaf = current.leaf;
  baseline.leaf_guard = current.leaf_guard;
  baseline.budgeted_recursion = current.budgeted_recursion;
  if (kind != LeafKind::Bounded || current.inline_scalar || !IsIntegerCallback(function)) {
    return;
  }
  auto& context = module.getContext();
  auto* integer = llvm::Type::getInt64Ty(context);
  const auto& target = module.getTargetTriple();
  llvm::Type* result = llvm::Type::getInt128Ty(context);
  if (target.isOSWindows() && target.getArch() == llvm::Triple::x86_64) {
    result = llvm::FixedVectorType::get(integer, 2);
  }
  auto* signature =
      llvm::FunctionType::get(result, {integer, llvm::PointerType::getUnqual(context)}, false);
  current.integer_callback = llvm::Function::Create(signature, llvm::GlobalValue::InternalLinkage,
                                                    prefix + "_integer_callback", module);
  baseline.integer_callback = current.integer_callback;
}

void ApplyInliningAttributes(GeneratedFunction& baseline,
                             GeneratedFunction& current,
                             InliningPlan plan,
                             NativeProgramMode mode) {
  if (!plan.enabled) {
    return;
  }
  baseline.body->addFnAttr(llvm::Attribute::AlwaysInline);
  current.body->addFnAttr(llvm::Attribute::AlwaysInline);
  if (mode == NativeProgramMode::Closed) {
    baseline.reflection_entry->addFnAttr(llvm::Attribute::AlwaysInline);
  }
}

std::expected<FunctionDeclarations, frontend::Diagnostics> DeclareFunctions(
    llvm::Module& module,
    const ir::Program& program,
    const std::string& module_name,
    NativeCodeTier tier,
    NativeDebugging debugging,
    NativeCodePlacement placement,
    std::span<const std::optional<BorrowedFunction>> borrows,
    std::span<const std::size_t> needed) {
  FunctionDeclarations declarations;
  auto& generated = declarations.baseline;
  auto& optimized = declarations.optimized;
  auto& borrowable = declarations.borrowable;
  const auto count = program.functions.size();
  generated.resize(count);
  optimized.resize(count);
  borrowable.resize(count);
  auto& context = module.getContext();
  const auto bind_scalars =
      debugging == NativeDebugging::Safepoints &&
      (program.native_mode == NativeProgramMode::Closed || tier == NativeCodeTier::Optimized);
  if (bind_scalars) {
    for (std::size_t index = 0; index < count; ++index) {
      const auto operand = ScalarCallbackOperand(program.functions[index]);
      generated[index].inline_scalar = operand;
      optimized[index].inline_scalar = operand;
    }
  }
  for (const auto index : needed) {
    const auto& function = program.functions.at(index);
    const auto operand = generated[index].inline_scalar;
    const auto body_type = BodySignature(module, function);
    if (!body_type) {
      return std::unexpected(MakeDiagnostics(program, body_type.error(), &function));
    }
    const auto symbol = [&](FunctionRole role) { return FunctionSymbol(module_name, role, index); };
    const auto prefix = symbol(FunctionRole::Prefix);
    auto* body = llvm::Function::Create(*body_type, llvm::GlobalValue::InternalLinkage,
                                        symbol(FunctionRole::Body), module);
    auto* pointer = llvm::PointerType::getUnqual(context);
    auto* wrapper_type = llvm::FunctionType::get(
        llvm::Type::getInt8Ty(context),
        {pointer, pointer, llvm::Type::getInt64Ty(context), pointer}, false);
    auto* entry = llvm::Function::Create(wrapper_type, llvm::GlobalValue::InternalLinkage,
                                         symbol(FunctionRole::Entry), module);
    auto* reflection_type = llvm::FunctionType::get(
        llvm::Type::getInt8Ty(context), {pointer, pointer, pointer, pointer, pointer}, false);
    auto* reflection = llvm::Function::Create(reflection_type, llvm::GlobalValue::InternalLinkage,
                                              symbol(FunctionRole::Reflection), module);
    const auto& target = module.getTargetTriple();
    if (placement == NativeCodePlacement::Linked &&
        (target.isOSBinFormatMachO() || target.isOSBinFormatCOFF())) {
      // 边界包装与 typed 主体分段，避免执行内部调用时一并驻留未调用的包装。
      // COFF 的 $ 后缀只参与链接排序；两种格式都不改变回调的调用契约或冷热属性。
      const auto* section = target.isOSBinFormatCOFF()
                                ? ".text$vela_entry"
                                : "__TEXT,__vela_entry,regular,pure_instructions";
      entry->setSection(section);
      reflection->setSection(section);
    }
    auto* resume = AddResumeEntry(module, function, symbol(FunctionRole::Resume));
    GeneratedFunction baseline{
        .body = body,
        .entry = entry,
        .reflection_entry = reflection,
        .resume = resume,
        .math_intrinsic = FindMathIntrinsic(function, program.layouts),
        .inline_scalar = operand,
    };
    auto current = baseline;
    // 仅可重载映像存在版本失效；封闭映像的调用目标和字段布局在实例期内固定。
    if (program.native_mode == NativeProgramMode::Reloadable && tier == NativeCodeTier::Optimized &&
        function.native_symbol.empty() && HasVersionAssumptions(function, program)) {
      current = AddRecoveryEntries(module, prefix, baseline);
    }
    const auto inlining =
        PlanInlining(function, index, program.native_mode, debugging, placement, tier);
    AddCallbackLeafEntries(module, prefix, function, inlining.leaf, baseline, current);
    ApplyInliningAttributes(baseline, current, inlining, program.native_mode);
    if (program.native_mode == NativeProgramMode::Reloadable && tier == NativeCodeTier::Optimized &&
        CanUseScalarFieldBody(function, program.native_mode, debugging)) {
      current.scalar_field_body = llvm::Function::Create(
          *body_type, llvm::GlobalValue::InternalLinkage, prefix + "_scalar_field_body", module);
      current.scalar_field_body->addFnAttr(llvm::Attribute::AlwaysInline);
    }
    if (const auto& borrow = borrows[index]) {
      baseline.borrowed = llvm::Function::Create(*body_type, llvm::GlobalValue::InternalLinkage,
                                                 symbol(FunctionRole::Borrowed), module);
      borrowable.at(index) = borrow->type;
      current.borrowed = baseline.borrowed;
    }
    const std::array typed_entries{
        body, current.body, current.leaf, baseline.borrowed, current.scalar_field_body,
    };
    SetTypedArguments(typed_entries, function);
    generated[index] = baseline;
    optimized[index] = current;
  }
  return declarations;
}

void DeclareRangeSpecializations(llvm::Module& module,
                                 const ir::Program& program,
                                 const ClosedArrayAccessFacts& facts,
                                 FunctionDeclarations& declarations) {
  if (facts.functions.empty()) {
    return;
  }
  declarations.specialized = declarations.optimized;
  for (std::size_t index = 0; index < program.functions.size(); ++index) {
    if (facts.roots[index] != 0 || facts.reachable[index] == 0) {
      continue;
    }
    const auto& source = program.functions[index];
    const auto& parameters = facts.parameters[index];
    const auto& ranges = facts.functions[index];
    bool constrained = false;
    for (std::size_t parameter = 0; parameter < source.parameter_count; ++parameter) {
      const auto& type = source.value_types[parameter];
      constrained |=
          (type.kind == Kind::Int && !type.nullable && !parameters[parameter].isFullSet()) ||
          (type.kind == Kind::Array &&
           (ranges.fixed_storage[parameter] != 0 ||
            ranges.element_bits[parameter] != kUnknownArrayElementBits));
    }
    if (!constrained) {
      continue;
    }
    auto& target = declarations.specialized[index];
    const auto create = [&](llvm::Function* entry) {
      if (entry == nullptr) {
        return entry;
      }
      auto* result =
          llvm::Function::Create(entry->getFunctionType(), llvm::GlobalValue::InternalLinkage,
                                 entry->getName() + "_range", module);
      result->copyAttributesFrom(entry);
      // 逐指令发射证明与 LLVM 参数域必须一致，否则内联后的循环守卫仍按完整整数域优化。
      // 仅特化入口消费这些事实；映像表中的通用入口保持原来的参数契约。
      for (std::size_t parameter = 0; parameter < source.parameter_count; ++parameter) {
        const auto& type = source.value_types[parameter];
        const auto& range = parameters[parameter];
        if (type.kind == Kind::Int && !type.nullable && !range.isFullSet() && !range.isEmptySet()) {
          result->addParamAttr(
              static_cast<unsigned>(parameter + 1),
              llvm::Attribute::get(module.getContext(), llvm::Attribute::Range, range));
        }
      }
      return result;
    };
    // 受约束版本不进入 NativeImage；运行时索引和 callable 仍只引用通用入口。
    target.body = create(target.body);
    target.leaf = create(target.leaf);
    target.leaf_guard = create(target.leaf_guard);
  }
}

std::vector<std::string> FunctionSymbols(const GeneratedFunction& baseline,
                                         const GeneratedFunction& optimized,
                                         const GeneratedFunction* specialized) {
  std::vector<std::string> symbols;
  const std::array functions{
      baseline.body,
      baseline.entry,
      baseline.reflection_entry,
      baseline.resume,
      baseline.recovery,
      baseline.resume_recovery,
      baseline.borrowed,
      optimized.body,
      optimized.scalar_field_body,
      optimized.resume,
      optimized.leaf,
      optimized.leaf_guard,
      optimized.host_leaf,
      optimized.host_guard,
      optimized.host_entry,
      optimized.host_wide_entry,
      optimized.host_entry_selector,
      optimized.integer_callback,
      specialized == nullptr ? nullptr : specialized->body,
      specialized == nullptr ? nullptr : specialized->leaf,
      specialized == nullptr ? nullptr : specialized->leaf_guard,
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
    const ArrayAccessFacts& array_access,
    const ClosedArrayAccessFacts& closed_access,
    const GlobalAccessFacts& global_access,
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
  const auto& target = optimized.at(index);
  auto emission_entries = tier == NativeCodeTier::Optimized && target.baseline_body == nullptr
                              ? std::span<const GeneratedFunction>(optimized)
                              : std::span<const GeneratedFunction>(generated);
  const auto emission_tier = target.baseline_body == nullptr ? tier : NativeCodeTier::Baseline;
  const auto has_closed_access = !closed_access.functions.empty();
  const auto has_range_specialization = declarations.HasRangeSpecialization(index);
  const auto range_root =
      has_closed_access && !has_range_specialization && closed_access.reachable[index] != 0;
  const auto generic_access = array_access.emission();
  auto range_access =
      has_closed_access ? closed_access.functions[index].emission() : ArrayEmissionFacts{};
  // 范围证明不扩大参数数组缓存，叶入口继续使用通用入口的物理帧预算。
  range_access.fixed_storage = generic_access.fixed_storage;
  range_access.reserved_pushes = generic_access.reserved_pushes;
  const auto root_access = range_root ? range_access : generic_access;
  if (range_root) {
    emission_entries = declarations.specialized;
  }
  const auto analysis = PlanFunctionAnalysis(program, index, debugging, emission_tier, escape,
                                             global_access.readonly_slots, declarations.borrowable);
  auto emitted = EmitFunction(module, program, calls, field_targets, root_access, global_access,
                              image, index, analysis, emission_entries, body_dependencies,
                              debugging, placement, emission_tier);
  if (!emitted) {
    return emitted;
  }
  if (has_range_specialization) {
    const auto specialized = EmitFunction(
        module, program, calls, field_targets, range_access, global_access, image, index, analysis,
        declarations.specialized, body_dependencies, debugging, placement, emission_tier);
    if (!specialized) {
      return specialized;
    }
    emitted->frame_bytes = std::max(emitted->frame_bytes, specialized->frame_bytes);
  }
  if (const auto& borrow = borrows[index]) {
    const auto borrowed =
        EmitBorrowedFunction(module, program, calls, field_targets, generic_access, global_access,
                             image, index, borrow->plan, generated, debugging, placement);
    if (!borrowed) {
      return std::unexpected(borrowed.error());
    }
    emitted->borrowed_frame_bytes = borrowed->frame_bytes;
    emitted->borrowed_shadow_slots = borrowed->shadow_slots;
    emitted->borrowed_root_stores = borrowed->root_stores;
  }
  if (target.baseline_body != nullptr) {
    const auto optimized_analysis =
        PlanFunctionAnalysis(program, index, debugging, NativeCodeTier::Optimized, escape,
                             global_access.readonly_slots, {});
    auto optimized_metrics =
        EmitFunction(module, program, calls, field_targets, generic_access, global_access, image,
                     index, optimized_analysis, optimized, body_dependencies, debugging, placement,
                     NativeCodeTier::Optimized);
    if (!optimized_metrics) {
      return optimized_metrics;
    }
    emitted = std::move(optimized_metrics);
  }
  const auto function_index = static_cast<std::uint32_t>(index);
  EmitWrapper(module, program, function_index, target, image, placement,
              NativeEntryContract::External);
  EmitWrapper(module, program, function_index, target, image, placement,
              NativeEntryContract::Reflection);
  return emitted;
}

void BindDynamicEntries(NativeModule& output,
                        std::span<llvm::Function* const> entries,
                        std::span<const GeneratedFunction> functions) {
  output.entry_functions.assign(entries.begin(), entries.end());
  for (const auto& function : functions) {
    auto* typed = function.body;
    output.typed_functions.push_back(typed);
    auto* callback = function.integer_callback;
    output.integer_callbacks.push_back(callback);
    if (callback != nullptr) {
      callback->setLinkage(llvm::GlobalValue::ExternalLinkage);
    }
    auto* reflection = function.reflection_entry;
    output.reflection_functions.push_back(reflection);
    if (reflection != nullptr) {
      reflection->setLinkage(llvm::GlobalValue::ExternalLinkage);
    }
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

void PlanRecursiveExpansion(NativeModule& output,
                            const ir::Program& program,
                            std::size_t index,
                            const NativeFunctionMetrics& metrics,
                            const FunctionDeclarations& declarations,
                            NativeCodePlacement placement,
                            NativeCodeTier tier) {
  if (!CanExpandRecursion(program, index, metrics, placement, tier)) {
    return;
  }
  const auto add = [&](const GeneratedFunction& function) {
    output.recursive_functions.push_back(function.budgeted_recursion ? function.leaf
                                                                     : function.body);
  };
  const auto& function = declarations.optimized[index];
  add(function);
  if (declarations.HasRangeSpecialization(index)) {
    add(declarations.specialized[index]);
  }
}

void RequireReflectionReceiverLayouts(const ir::Program& program,
                                      std::span<const GeneratedFunction> functions,
                                      NativeDebugging debugging,
                                      NativeCodeTier tier,
                                      NativeImageData& image) {
  if (program.native_mode != NativeProgramMode::Reloadable || tier != NativeCodeTier::Optimized) {
    return;
  }
  for (std::size_t index = 0; index < functions.size(); ++index) {
    const auto& function = functions[index];
    // 复用函数没有本次 LLVM 声明，仍需从其规范 IR 恢复入口契约。
    if (function.scalar_field_body != nullptr ||
        (function.body == nullptr &&
         CanUseScalarFieldBody(program.functions[index], program.native_mode, debugging))) {
      image.RequireReflectionReceiverLayout(index);
    }
  }
}

std::vector<std::uint8_t> ReflectedFunctions(const ir::Program& program,
                                             NativeCodePlacement placement) {
  const auto closed = IsClosedLinkedProgram(program.native_mode, placement);
  std::vector<std::uint8_t> functions(program.functions.size(), closed ? 0 : 1);
  if (!closed) {
    return functions;
  }
  std::vector<std::uint8_t> selectors(program.selector_declarations.size());
  for (const auto& member : program.reflection_members) {
    if (!reflection::IsCallable(member.kind) || member.target == kInvalidFunctionTarget) {
      continue;
    }
    if ((member.flags & reflection::kVirtual) != 0) {
      selectors[member.target] = 1;
    } else {
      functions[member.target] = 1;
    }
  }
  for (const auto& type : program.classes) {
    for (std::size_t selector = 0; selector < type.dispatch_targets.size(); ++selector) {
      const auto target = type.dispatch_targets[selector];
      if (selectors[selector] != 0 && target != kInvalidFunctionTarget) {
        functions[target] = 1;
      }
    }
  }
  return functions;
}

std::expected<llvm::Constant*, std::string> EmitModuleImage(
    NativeModule& output,
    const ir::Program& program,
    const FunctionDeclarations& declarations,
    const std::string& module_name,
    NativeDebugging debugging,
    NativeCodeTier tier) {
  auto& module = *output.module;
  const auto& generated = declarations.baseline;
  const auto& optimized = declarations.optimized;
  const auto placement = output.placement;
  std::vector<llvm::Function*> entries;
  std::vector<llvm::Function*> reflection_entries;
  std::vector<llvm::Function*> host_entries;
  std::vector<llvm::Function*> callbacks;
  const auto count = generated.size();
  entries.reserve(count);
  reflection_entries.reserve(count);
  host_entries.reserve(count);
  callbacks.reserve(count);
  const auto reflected = ReflectedFunctions(program, placement);
  for (std::size_t index = 0; index < count; ++index) {
    const auto& function = generated[index];
    entries.push_back(function.entry);
    // 封闭映像只为实际描述的反射目标保留运行时入口；普通宿主调用使用独立 external ABI。
    // 编译器直接绑定的反射调用仍通过 LLVM 引用保留其包装，动态代码继续发布完整入口表。
    reflection_entries.push_back(reflected[index] != 0 ? function.reflection_entry : nullptr);
    host_entries.push_back(function.host_entry_selector);
    callbacks.push_back(function.integer_callback);
  }
  auto description = DescribeNativeImage(program);
  RequireReflectionReceiverLayouts(program, optimized, debugging, tier, *description);
  for (std::size_t index = 0; index < generated.size(); ++index) {
    if (const auto operand = generated[index].inline_scalar) {
      description->BindInlineScalarCallback(index, *operand);
    }
  }
  llvm::Constant* image =
      llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*output.context));
  if (placement == NativeCodePlacement::Dynamic &&
      program.native_mode == NativeProgramMode::Reloadable) {
    output.image_data = std::move(description);
    BindDynamicEntries(output, entries, optimized);
  } else {
    const auto emitted = EmitNativeImage(module, *description->image(),
                                         {
                                             .external = entries,
                                             .reflection = reflection_entries,
                                             .host = host_entries,
                                             .integer_callbacks = callbacks,
                                         },
                                         module_name);
    if (!emitted) {
      return std::unexpected(emitted.error());
    }
    output.image_symbol = emitted->symbol;
    image = emitted->data;
  }
  return image;
}

void SetModuleTargetAttributes(NativeModule& output) {
  auto& module = *output.module;
  for (auto& function : module) {
    if (!function.isDeclaration()) {
      // 标准 LLVM 目标属性同时参与机器码生成和对象缓存身份，避免跨 CPU 误用产物。
      if (!function.hasFnAttribute("target-cpu")) {
        function.addFnAttr("target-cpu", output.machine->getTargetCPU());
      }
      if (!function.hasFnAttribute("target-features")) {
        function.addFnAttr("target-features", output.machine->getTargetFeatureString());
      }
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

std::expected<std::string, std::string> TargetDataLayout() {
  auto target = CreateTarget(NativeCodePlacement::Dynamic, NativeOptimization::None);
  if (!target) {
    return std::unexpected(std::move(target.error()));
  }
  return (*target)->createDataLayout().getStringRepresentation();
}

NativeModule::NativeModule(NativeProgramMode mode,
                           NativeCodePlacement placement,
                           NativeOptimization optimization)
    : mode(mode), placement(placement), optimization(optimization) {}
NativeModule::~NativeModule() = default;
NativeModule::NativeModule(NativeModule&&) noexcept = default;

std::expected<BaselineCompilationPlan, frontend::Diagnostics> PlanBaselineCompilation(
    const ir::Program& program,
    NativeDebugging debugging,
    EscapeOptimization escape,
    NativeOptimization optimization) {
  escape = EffectiveEscape(debugging, escape);
  BaselineCompilationPlan plan(debugging, escape, optimization);
  plan.cpu = TargetCpu(NativeCodePlacement::Dynamic);
  plan.calls = PlanNativeCalls(program, NativeCodeTier::Baseline, debugging);
  plan.fields = FindFieldTargets(program);
  plan.arrays = AnalyzeArrayAccess(program, ArrayAnalysisMode::Elements);
  plan.borrows = FunctionBorrows(program, escape);
  auto target = CreateTarget(NativeCodePlacement::Dynamic, optimization, plan.cpu);
  if (!target) {
    return std::unexpected(MakeDiagnostics(program, target.error(), nullptr));
  }
  plan.data_layout = (*target)->createDataLayout().getStringRepresentation();
  for (std::size_t index = 0; index < program.functions.size(); ++index) {
    const auto& function = program.functions[index];
    const auto symbol = [&](FunctionRole role) {
      return FunctionSymbol(std::string(kBaselineModuleName), role, index);
    };
    auto external = symbol(FunctionRole::Entry);
    auto reflection = symbol(FunctionRole::Reflection);
    auto typed = symbol(FunctionRole::Body);
    FunctionEntrySymbols entries{
        .external = external,
        .reflection = reflection,
        .typed = typed,
        .implementations =
            {
                std::move(typed),
                std::move(external),
                std::move(reflection),
            },
    };
    if (HasResumeEntry(function)) {
      entries.implementations.push_back(symbol(FunctionRole::Resume));
    }
    if (plan.borrows[index]) {
      entries.implementations.push_back(symbol(FunctionRole::Borrowed));
    }
    plan.symbols.push_back(std::move(entries));
    std::vector<std::size_t> needed{index};
    ForEachDirectCallee(function, plan.calls.direct_calls[index],
                        [&](std::size_t callee) { needed.push_back(callee); });
    std::ranges::sort(needed);
    const auto duplicates = std::ranges::unique(needed);
    needed.erase(duplicates.begin(), duplicates.end());
    plan.needed.push_back(std::move(needed));
  }
  return plan;
}

std::expected<NativeModule, frontend::Diagnostics> GenerateBaselineFunction(
    const ir::Program& program,
    const BaselineCompilationPlan& plan,
    std::size_t function) {
  const auto debugging = plan.debugging;
  const auto escape = plan.escape;
  const auto optimization = plan.optimization;
  NativeModule output(program.native_mode, NativeCodePlacement::Dynamic, optimization);
  auto target = CreateTarget(NativeCodePlacement::Dynamic, optimization, plan.cpu);
  if (!target) {
    return std::unexpected(MakeDiagnostics(program, target.error(), nullptr));
  }
  output.machine = std::move(*target);
  InitializeModule(output, std::string(kBaselineModuleName));
  auto& module = *output.module;
  auto declarations = DeclareFunctions(
      module, program, std::string(kBaselineModuleName), NativeCodeTier::Baseline, debugging,
      NativeCodePlacement::Dynamic, plan.borrows, plan.needed[function]);
  if (!declarations) {
    return std::unexpected(std::move(declarations.error()));
  }
  auto dependencies = plan.calls.dependencies[function];
  const GlobalAccessFacts globals;
  const auto emitted = EmitDeclaredFunction(
      module, program, plan.calls, plan.fields, plan.arrays.generic[function], plan.arrays.closed,
      globals, llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*output.context)),
      function, *declarations, dependencies, debugging, NativeCodeTier::Baseline, escape,
      NativeCodePlacement::Dynamic, plan.borrows);
  if (!emitted) {
    return std::unexpected(MakeDiagnostics(program, emitted.error(), &program.functions[function]));
  }
  SetModuleTargetAttributes(output);
  return output;
}

std::expected<NativeModule, frontend::Diagnostics> Generate(const ir::Program& program,
                                                            const NativeCallPlan& calls,
                                                            const FieldTargets& field_targets,
                                                            const std::string& module_name,
                                                            NativeDebugging debugging,
                                                            NativeCodeTier tier,
                                                            EscapeOptimization escape,
                                                            NativeCodePlacement placement,
                                                            NativeOptimization optimization,
                                                            std::span<const std::uint8_t> reused) {
  escape = EffectiveEscape(debugging, escape);
  const auto diagnostic = [&](std::string message, const ir::Function* function = nullptr) {
    return std::unexpected(MakeDiagnostics(program, std::move(message), function));
  };
  auto target = CreateTarget(placement, optimization);
  if (!target) {
    return diagnostic(target.error());
  }
  NativeModule output(program.native_mode, placement, optimization);
  output.machine = std::move(*target);
  output.body_dependencies = calls.dependencies;
  InitializeModule(output, module_name);
  auto& module = *output.module;
  const auto borrows = FunctionBorrows(program, escape);
  auto declarations = DeclareFunctions(module, program, module_name, tier, debugging, placement,
                                       borrows, NeededFunctions(program, calls, reused));
  if (!declarations) {
    return std::unexpected(declarations.error());
  }
  auto array_mode = ArrayAnalysisMode::Elements;
  if (IsClosedLinkedProgram(program.native_mode, placement)) {
    array_mode = debugging == NativeDebugging::Safepoints ? ArrayAnalysisMode::Ranges
                                                          : ArrayAnalysisMode::FixedStorage;
  }
  // 在逃逸改写前分析一次规范程序，同一调用域的所有入口共享不可变事实。
  const auto array_access = AnalyzeArrayAccess(program, array_mode);
  const auto& closed_access = array_access.closed;
  HostEntryPlan host_plan{.needed = {}, .globals = {}};
  if (IsClosedLinkedProgram(program.native_mode, placement) &&
      debugging == NativeDebugging::Safepoints) {
    host_plan = DeclareHostEntries(module, program, *declarations);
  }
  DeclareRangeSpecializations(module, program, closed_access, *declarations);
  const auto& generated = declarations->baseline;
  const auto& optimized = declarations->optimized;
  const auto emitted_image =
      EmitModuleImage(output, program, *declarations, module_name, debugging, tier);
  if (!emitted_image) {
    return diagnostic(emitted_image.error());
  }
  auto* image = *emitted_image;
  std::vector<NativeFunctionMetrics> metrics;
  const auto global_access = AnalyzeGlobalAccess(program, placement);
  for (std::size_t index = 0; index < program.functions.size(); ++index) {
    if (!reused.empty() && reused[index] != 0) {
      metrics.emplace_back();
      continue;
    }
    const auto& function = program.functions.at(index);
    auto& dependencies = output.body_dependencies.at(index);
    auto emitted =
        EmitDeclaredFunction(module, program, calls, field_targets, array_access.generic.at(index),
                             closed_access, global_access, image, index, *declarations,
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
    PlanRecursiveExpansion(output, program, index, *emitted, *declarations, placement, tier);
  }
  output.function_metrics = std::move(metrics);
  if (!host_plan.needed.empty()) {
    FinalizeHostEntries(module, program, host_plan, declarations->optimized);
  }
  output.function_symbols.reserve(generated.size());
  const auto& specialized = declarations->specialized;
  const auto has_specialized = !specialized.empty();
  for (std::size_t index = 0; index < generated.size(); ++index) {
    output.function_symbols.push_back(FunctionSymbols(
        generated[index], optimized[index], has_specialized ? &specialized[index] : nullptr));
  }
  if (optimization == NativeOptimization::Release &&
      IsClosedLinkedProgram(program.native_mode, placement) &&
      module.getTargetTriple().getArch() == llvm::Triple::x86_64 &&
      debugging == NativeDebugging::Safepoints) {
    output.target_functions = PlanTargetVersions(program, calls, *declarations);
  }
  SetModuleTargetAttributes(output);
  return output;
}

std::expected<void, std::string> Finalize(NativeModule& generated) {
  auto& module = *generated.module;
  std::string verification;
  llvm::raw_string_ostream verifier(verification);
  if (llvm::verifyModule(module, &verifier)) {
    return std::unexpected(verification);
  }
  if (generated.optimization == NativeOptimization::Release) {
    for (auto* function : generated.recursive_functions) {
      InlineRecursion(*function);
    }
    generated.recursive_functions.clear();
    EmitTargetVersions(module, generated.target_functions, generated.function_symbols);
    generated.target_functions.clear();
    Optimize(module, *generated.machine, generated.mode, generated.placement);
  } else if (generated.optimization == NativeOptimization::Baseline) {
    // 基线只整理本函数的 SSA 和控制流，不做模块内联、向量化或推测性特化。
    llvm::LoopAnalysisManager loops;
    llvm::FunctionAnalysisManager functions;
    llvm::CGSCCAnalysisManager calls;
    llvm::ModuleAnalysisManager modules;
    llvm::PassBuilder passes(generated.machine.get());
    passes.registerLoopAnalyses(loops);
    passes.registerFunctionAnalyses(functions);
    passes.registerCGSCCAnalyses(calls);
    passes.registerModuleAnalyses(modules);
    passes.crossRegisterProxies(loops, functions, calls, modules);
    auto canonicalization = passes.buildFunctionSimplificationPipeline(
        llvm::OptimizationLevel::O1, llvm::ThinOrFullLTOPhase::None);
    for (auto& function : module) {
      if (function.isDeclaration()) {
        continue;
      }
      canonicalization.run(function, functions);
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
