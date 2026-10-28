#include "compiler/aot/native_compiler.h"
#include "compiler/aot/function_emitter.h"
#include "compiler/aot/native_values.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Triple.h>

#include "base/fingerprint.h"
#include "base/opcode.h"
#include "compiler/aot/native_export_emitter.h"
#include "compiler/aot/native_image_emitter.h"
#include "compiler/aot/native_roots.h"
#include "compiler/aot/reachability.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/type_checker.h"
#include "compiler/ir/ir.h"
#include "runtime/native_image.h"
#include "runtime/standard_library.h"

namespace vela::compiler::aot {
namespace {

std::expected<std::unique_ptr<llvm::TargetMachine>, std::string> CreateTarget() {
  static const bool initialized = [] {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    return true;
  }();
  static_cast<void>(initialized);
  // 编译器、运行时和产物必须使用同一个配置目标。LLVM 的默认 triple 可能带有
  // 与当前 SDK 不同的 Darwin 版本，直接读取它会让生成镜像无法在同一进程实例化。
  const auto triple = std::string(runtime::NativeTargetIdentity());
  std::string error;
  const auto* target = llvm::TargetRegistry::lookupTarget(triple, error);
  if (target == nullptr) {
    return std::unexpected(std::move(error));
  }
  llvm::TargetOptions options;
  std::unique_ptr<llvm::TargetMachine> machine(
      target->createTargetMachine(llvm::Triple(triple), "generic", "", options, llvm::Reloc::PIC_));
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
  passes.registerModuleAnalyses(modules);
  passes.registerCGSCCAnalyses(calls);
  passes.registerFunctionAnalyses(functions);
  passes.registerLoopAnalyses(loops);
  passes.crossRegisterProxies(loops, functions, calls, modules);
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

std::expected<absl::flat_hash_set<std::string>, std::string> SelectExports(
    const ir::Program& program,
    std::span<const std::string> names) {
  absl::flat_hash_map<std::string_view, bool> functions;
  for (const auto& function : program.functions) {
    functions.emplace(function.name, function.is_exported);
  }
  absl::flat_hash_set<std::string> selected;
  for (const auto& name : names) {
    const auto found = functions.find(name);
    if (found == functions.end() || !found->second) {
      return std::unexpected("AOT export must name a public entry-module function: " + name);
    }
    selected.insert(name);
  }
  return selected;
}

std::expected<llvm::FunctionType*, std::string> BodySignature(llvm::Module& module,
                                                              const ir::Function& function) {
  if (function.return_type.kind == Kind::Invalid ||
      (function.is_async && (!function.signature || !function.signature->result))) {
    return std::unexpected("AOT requires upstream return types and async runtime signatures");
  }
  auto& context = module.getContext();
  std::vector<llvm::Type*> parameters{llvm::PointerType::getUnqual(context)};
  std::uint64_t argument_bytes = 0;
  for (std::size_t parameter = 0; parameter < function.parameter_count; ++parameter) {
    if (parameter >= function.value_types.size() ||
        function.value_types.at(parameter).kind == Kind::Invalid) {
      return std::unexpected("AOT function " + function.name +
                             " requires an upstream type for parameter " +
                             std::to_string(parameter));
    }
    parameters.push_back(ValueType(context, function.value_types.at(parameter)));
    argument_bytes += module.getDataLayout().getTypeAllocSize(parameters.back());
  }
  if (argument_bytes > kMaximumNativeStackBytes) {
    return std::unexpected("AOT typed arguments exceed the native call stack budget");
  }
  parameters.push_back(llvm::PointerType::getUnqual(context));
  return llvm::FunctionType::get(llvm::Type::getInt8Ty(context), parameters, false);
}

std::expected<std::uint32_t, std::string> SelectMain(const ir::Program& program) {
  std::optional<std::uint32_t> main;
  for (std::size_t index = 0; index < program.functions.size(); ++index) {
    const auto& function = program.functions.at(index);
    if (function.name == "main") {
      if (function.parameter_count != 0) {
        return std::unexpected("build does not accept main function parameters yet");
      }
      main = static_cast<std::uint32_t>(index);
    }
  }
  if (!main) {
    return std::unexpected("build requires a zero-argument main function");
  }
  return *main;
}

std::expected<void, std::string> CheckStandaloneImports(const ir::Program& program) {
  for (const auto& function : program.functions) {
    if (function.native_symbol.empty()) {
      continue;
    }
    const auto binding = runtime::FindStandardNative(function.native_symbol);
    if (binding == nullptr) {
      return std::unexpected("standalone executable cannot use custom host binding '" +
                             function.native_symbol + "'; emit a library for host embedding");
    }
    const auto& signature = function.signature;
    if (!signature) {
      std::unreachable();
    }
    if (!binding->IsSignatureCompatible(*signature, program.layouts.types)) {
      return std::unexpected("external declaration does not match standard native signature for '" +
                             function.native_symbol + "'");
    }
  }
  return {};
}

void EmitMain(llvm::Module& module, llvm::Function& image, std::uint32_t function) {
  auto& context = module.getContext();
  auto* integer = llvm::Type::getInt32Ty(context);
  const auto invoke = module.getOrInsertFunction(
      "vela_native_v1_main",
      llvm::FunctionType::get(integer, {llvm::PointerType::getUnqual(context), integer}, false));
  auto* entry = llvm::Function::Create(llvm::FunctionType::get(integer, false),
                                       llvm::GlobalValue::ExternalLinkage, "main", module);
  llvm::IRBuilder<> builder(llvm::BasicBlock::Create(context, "entry", entry));
  auto* descriptor = builder.CreateCall(&image);
  builder.CreateRet(builder.CreateCall(invoke, {descriptor, builder.getInt32(function)}));
}

std::expected<std::optional<std::uint32_t>, std::string> PrepareNativeProgram(
    ir::Program& program,
    const NativeOptions& options) {
  std::optional<std::uint32_t> main;
  if (options.entry_mode == NativeEntryMode::Executable) {
    const auto entry = SelectMain(program);
    if (!entry) {
      return std::unexpected(entry.error());
    }
    main = *entry;
  }
  PruneNativeProgram(program, options.exports, main);
  if (main) {
    if (const auto imports = CheckStandaloneImports(program); !imports) {
      return std::unexpected(imports.error());
    }
  }
  return main;
}

}  // namespace

std::string NativeToolchainBuildIdentity(NativeOptimization optimization) {
  std::string identity(runtime::NativeRuntimeAbiIdentity());
  identity += optimization == NativeOptimization::Release ? "/O2" : "/O0";
  return ContentFingerprint(identity).Hex();
}

std::expected<NativeArtifact, frontend::Diagnostics> Compile(ir::Program program,
                                                             const NativeOptions& options) {
  const auto diagnostic = [&](std::string message, const ir::Function* function = nullptr) {
    return std::unexpected(MakeDiagnostics(program, std::move(message), function));
  };
  auto target = CreateTarget();
  if (!target) {
    return diagnostic(target.error());
  }
  auto& machine = **target;
  const auto selected = SelectExports(program, options.exports);
  if (!selected) {
    return diagnostic(selected.error());
  }
  const auto prepared = PrepareNativeProgram(program, options);
  if (!prepared) {
    return diagnostic(prepared.error());
  }
  const auto main = *prepared;
  llvm::LLVMContext context;
  llvm::Module module(options.module_name, context);
  module.setTargetTriple(machine.getTargetTriple());
  module.setDataLayout(machine.createDataLayout());
  std::vector<GeneratedFunction> generated;
  std::vector<NativeExport> exports;
  std::vector<std::uint8_t> export_flags(program.functions.size(), 0);
  for (std::size_t index = 0; index < program.functions.size(); ++index) {
    const auto& function = program.functions.at(index);
    const auto body_type = BodySignature(module, function);
    if (!body_type) {
      return diagnostic(body_type.error(), &function);
    }
    auto* body =
        llvm::Function::Create(*body_type, llvm::GlobalValue::InternalLinkage,
                               options.module_name + "_body_" + std::to_string(index), module);
    auto* pointer = llvm::PointerType::getUnqual(context);
    auto* wrapper_type = llvm::FunctionType::get(
        llvm::Type::getInt8Ty(context),
        {pointer, pointer, llvm::Type::getInt64Ty(context), pointer}, false);
    const auto symbol = options.module_name + "_entry_" + std::to_string(index);
    auto* entry =
        llvm::Function::Create(wrapper_type, llvm::GlobalValue::InternalLinkage, symbol, module);
    if (selected->contains(function.name)) {
      export_flags.at(index) = 1;
      exports.push_back({
          .name = function.name,
          .symbol = options.module_name + "_export_" + std::to_string(index),
          .function = static_cast<std::uint32_t>(index),
      });
    }
    llvm::Function* resume = nullptr;
    if (function.is_async &&
        std::ranges::any_of(function.instructions, [](const auto& instruction) {
          return instruction.opcode == Opcode::Await;
        })) {
      auto* resume_type = llvm::FunctionType::get(llvm::Type::getInt8Ty(context),
                                                  {pointer, pointer, pointer}, false);
      resume =
          llvm::Function::Create(resume_type, llvm::GlobalValue::InternalLinkage,
                                 options.module_name + "_resume_" + std::to_string(index), module);
    }
    generated.push_back({.body = body, .entry = entry, .resume = resume});
  }
  std::vector<NativeFunctionMetrics> metrics;
  for (std::size_t index = 0; index < program.functions.size(); ++index) {
    const auto& function = program.functions.at(index);
    auto emitted = EmitFunction(module, program, index, generated);
    if (!emitted) {
      return diagnostic(emitted.error(), &function);
    }
    metrics.push_back(*emitted);
    const auto& native_function = generated.at(index);
    EmitWrapper(module, function, native_function);
  }
  std::vector<llvm::Function*> entries;
  entries.reserve(generated.size());
  for (const auto& function : generated) {
    entries.push_back(function.entry);
  }
  const auto image_symbol =
      EmitNativeImage(module, program, entries, export_flags, options.module_name);
  if (!image_symbol) {
    return diagnostic(image_symbol.error());
  }
  auto export_interface = EmitNativeExports(
      module, program, exports, *module.getFunction(*image_symbol), options.module_name);
  if (!export_interface) {
    return diagnostic(export_interface.error());
  }
  if (main) {
    EmitMain(module, *module.getFunction(*image_symbol), *main);
  }
  std::string verification;
  llvm::raw_string_ostream verifier(verification);
  if (llvm::verifyModule(module, &verifier)) {
    return diagnostic(verification);
  }
  if (options.optimization == NativeOptimization::Release) {
    Optimize(module, machine);
  }
  if (llvm::verifyModule(module, &verifier)) {
    return diagnostic(verification);
  }
  std::string llvm_ir;
  llvm::raw_string_ostream ir_output(llvm_ir);
  module.print(ir_output, nullptr);
  llvm::SmallVector<char, 0> bytes;
  llvm::raw_svector_ostream stream(bytes);
  llvm::legacy::PassManager codegen;
  if (machine.addPassesToEmitFile(codegen, stream, nullptr, llvm::CodeGenFileType::ObjectFile)) {
    return diagnostic("native target cannot emit object files");
  }
  codegen.run(module);
  std::vector<std::byte> object(bytes.size());
  std::ranges::transform(bytes, object.begin(), [](char value) {
    return static_cast<std::byte>(static_cast<unsigned char>(value));
  });
  return NativeArtifact{
      .object = std::move(object),
      .llvm_ir = std::move(llvm_ir),
      .target = machine.getTargetTriple().str(),
      .exports = std::move(exports),
      .image_symbol = *image_symbol,
      .header = std::move(export_interface->header),
      .instantiate_symbol = std::move(export_interface->instantiate_symbol),
      .optimization = options.optimization,
      .program = std::move(program),
      .function_metrics = std::move(metrics),
  };
}

}  // namespace vela::compiler::aot
