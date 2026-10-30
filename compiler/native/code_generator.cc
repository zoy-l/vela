#include "compiler/native/code_generator.h"

#include <algorithm>
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
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/Transforms/Scalar/LoopPassManager.h>
#include <llvm/Transforms/Scalar/SimpleLoopUnswitch.h>

#include "base/native_program_mode.h"
#include "base/opcode.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/ir/ir.h"
#include "compiler/native/function_emitter.h"
#include "compiler/native/integer_loop_optimizer.h"
#include "compiler/native/native_image_emitter.h"
#include "compiler/native/native_roots.h"
#include "compiler/native/native_values.h"
#include "runtime/native_image.h"

namespace vela::compiler::native {
namespace {

std::expected<std::unique_ptr<llvm::TargetMachine>, std::string> CreateTarget(
    NativeProgramMode mode) {
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
  std::unique_ptr<llvm::TargetMachine> machine(target->createTargetMachine(
      llvm::Triple(triple), "generic", "", options, llvm::Reloc::PIC_, std::nullopt,
      llvm::CodeGenOptLevel::Default, mode == NativeProgramMode::Reloadable));
  if (machine == nullptr) {
    return std::unexpected("unable to create native target machine");
  }
  return machine;
}

void Optimize(llvm::Module& module, llvm::TargetMachine& machine) {
  llvm::LoopAnalysisManager loops;
  llvm::FunctionAnalysisManager functions;
  llvm::CGSCCAnalysisManager calls;
  llvm::ModuleAnalysisManager modules;
  llvm::PassBuilder passes(&machine);
  // 宿主调用约定随实例固定；允许 O2 克隆循环，将 direct/checked 分派移到循环外。
  // 使用 LLVM 自带的体积阈值控制复制，不改变循环内错误和逻辑栈帧语义。
  passes.registerLateLoopOptimizationsEPCallback(
      [](llvm::LoopPassManager& pipeline, llvm::OptimizationLevel /*unused*/) {
        pipeline.addPass(llvm::SimpleLoopUnswitchPass(true));
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
  auto pipeline = passes.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2);
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
  if (function.return_type.kind == Kind::Invalid ||
      (function.is_async && (!function.signature || !function.signature->result))) {
    return std::unexpected(
        "native code requires upstream return types and async runtime signatures");
  }
  auto& context = module.getContext();
  std::vector<llvm::Type*> parameters{llvm::PointerType::getUnqual(context)};
  std::uint64_t argument_bytes = 0;
  for (std::size_t parameter = 0; parameter < function.parameter_count; ++parameter) {
    if (parameter >= function.value_types.size() ||
        function.value_types.at(parameter).kind == Kind::Invalid) {
      return std::unexpected("native function " + function.name +
                             " requires an upstream type for parameter " +
                             std::to_string(parameter));
    }
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

}  // namespace

void InitializeTarget() {
  static const bool initialized = [] {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    return true;
  }();
  static_cast<void>(initialized);
}

NativeModule::NativeModule() = default;
NativeModule::~NativeModule() = default;
NativeModule::NativeModule(NativeModule&&) noexcept = default;

std::expected<NativeModule, frontend::Diagnostics> Generate(const ir::Program& program,
                                                            const std::string& module_name,
                                                            std::span<const std::uint8_t> exports,
                                                            NativeDebugging debugging,
                                                            NativeCodeTier tier) {
  const auto diagnostic = [&](std::string message, const ir::Function* function = nullptr) {
    return std::unexpected(MakeDiagnostics(program, std::move(message), function));
  };
  auto target = CreateTarget(program.native_mode);
  if (!target) {
    return diagnostic(target.error());
  }
  NativeModule output;
  output.context = std::make_unique<llvm::LLVMContext>();
  output.machine = std::move(*target);
  output.module = std::make_unique<llvm::Module>(module_name, *output.context);
  auto& context = *output.context;
  auto& module = *output.module;
  module.setTargetTriple(output.machine->getTargetTriple());
  module.setDataLayout(output.machine->createDataLayout());
  module.setCodeModel(output.machine->getCodeModel());
  std::vector<GeneratedFunction> generated;
  std::vector<GeneratedFunction> optimized;
  for (std::size_t index = 0; index < program.functions.size(); ++index) {
    const auto& function = program.functions.at(index);
    const auto body_type = BodySignature(module, function);
    if (!body_type) {
      return diagnostic(body_type.error(), &function);
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
    llvm::Function* resume = nullptr;
    if (function.is_async &&
        std::ranges::any_of(function.instructions, [](const auto& instruction) {
          return instruction.opcode == Opcode::Await;
        })) {
      auto* resume_type =
          llvm::FunctionType::get(llvm::Type::getInt8Ty(context), {pointer, pointer}, false);
      resume = llvm::Function::Create(resume_type, llvm::GlobalValue::InternalLinkage,
                                      module_name + "_resume_" + std::to_string(index), module);
    }
    GeneratedFunction baseline{.body = body, .entry = entry, .resume = resume};
    auto current = baseline;
    if (tier == NativeCodeTier::Optimized && function.native_symbol.empty()) {
      current = AddRecoveryEntries(module, module_name + '_' + std::to_string(index), baseline);
    }
    generated.push_back(baseline);
    optimized.push_back(current);
  }
  std::vector<llvm::Function*> entries;
  entries.reserve(generated.size());
  for (const auto& function : generated) {
    entries.push_back(function.entry);
  }
  const auto image = EmitNativeImage(module, program, entries, exports, module_name);
  if (!image) {
    return diagnostic(image.error());
  }
  std::vector<NativeFunctionMetrics> metrics;
  for (std::size_t index = 0; index < program.functions.size(); ++index) {
    const auto& function = program.functions.at(index);
    auto emitted = EmitFunction(module, program, image->data, index, generated, debugging);
    if (!emitted) {
      return diagnostic(emitted.error(), &function);
    }
    if (optimized.at(index).baseline_body != nullptr) {
      auto optimized_metrics = EmitFunction(module, program, image->data, index, optimized,
                                            debugging, NativeCodeTier::Optimized);
      if (!optimized_metrics) {
        return diagnostic(optimized_metrics.error(), &function);
      }
      emitted = std::move(optimized_metrics);
    }
    metrics.push_back(*emitted);
    EmitWrapper(module, function, static_cast<std::uint32_t>(index), optimized.at(index),
                image->data);
  }
  output.image_symbol = image->symbol;
  output.function_metrics = std::move(metrics);
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
    Optimize(module, *generated.machine);
  }
  if (llvm::verifyModule(module, &verifier)) {
    return std::unexpected(verification);
  }
  return {};
}

std::expected<std::vector<std::byte>, std::string> EmitObject(NativeModule& generated) {
  llvm::SmallVector<char, 0> bytes;
  llvm::raw_svector_ostream stream(bytes);
  llvm::legacy::PassManager codegen;
  if (generated.machine->addPassesToEmitFile(codegen, stream, nullptr,
                                             llvm::CodeGenFileType::ObjectFile)) {
    return std::unexpected("native target cannot emit object files");
  }
  codegen.run(*generated.module);
  std::vector<std::byte> object(bytes.size());
  std::ranges::transform(bytes, object.begin(), [](char value) {
    return static_cast<std::byte>(static_cast<unsigned char>(value));
  });
  return object;
}

}  // namespace vela::compiler::native
