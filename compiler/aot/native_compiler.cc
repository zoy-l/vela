#include "compiler/aot/native_compiler.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>

#include "base/fingerprint.h"
#include "base/native_program_mode.h"
#include "compiler/aot/native_call_optimizer.h"
#include "compiler/aot/native_export_emitter.h"
#include "compiler/aot/native_type_optimizer.h"
#include "compiler/aot/reachability.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/ir/ir.h"
#include "compiler/native/code_generator.h"
#include "compiler/native/native_roots.h"
#include "runtime/native_image.h"
#include "runtime/standard_library.h"

namespace vela::compiler::aot {
namespace {

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
  ElideNativeResultBindings(program);
  DevirtualizeNativeCalls(program);
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
  const auto diagnostic = [](std::string message) {
    return std::unexpected(frontend::Diagnostics{
        frontend::Diagnostic{.location = {}, .message = std::move(message), .source_path = {}},
    });
  };
  if (program.native_mode != NativeProgramMode::Closed) {
    return diagnostic("AOT requires a closed native program");
  }
  const auto selected = SelectExports(program, options.exports);
  if (!selected) {
    return diagnostic(selected.error());
  }
  using Clock = std::chrono::steady_clock;
  NativeCompileTimings timings;
  const auto preparing = Clock::now();
  const auto prepared = PrepareNativeProgram(program, options);
  timings.preparation = Clock::now() - preparing;
  if (!prepared) {
    return diagnostic(prepared.error());
  }
  const auto main = *prepared;
  std::vector<NativeExport> exports;
  std::vector<std::uint8_t> export_flags(program.functions.size(), 0);
  for (std::size_t index = 0; index < program.functions.size(); ++index) {
    const auto& function = program.functions.at(index);
    if (selected->contains(function.name)) {
      export_flags.at(index) = 1;
      exports.push_back({
          .name = function.name,
          .symbol = options.module_name + "_export_" + std::to_string(index),
          .function = static_cast<std::uint32_t>(index),
      });
    }
  }
  const auto generating = Clock::now();
  auto generated = native::Generate(program, options.module_name, export_flags,
                                    native::NativeDebugging::Safepoints, NativeCodeTier::Baseline,
                                    options.escape, options.placement);
  if (!generated) {
    return std::unexpected(std::move(generated.error()));
  }
  timings.generation = Clock::now() - generating;
  auto& module = *generated->module;
  auto& machine = *generated->machine;
  auto export_interface = EmitNativeExports(
      module, program, exports, *module.getFunction(generated->image_symbol), options.module_name);
  if (!export_interface) {
    return diagnostic(export_interface.error());
  }
  if (main) {
    EmitMain(module, *module.getFunction(generated->image_symbol), *main);
  }
  const auto optimizing = Clock::now();
  if (const auto finalized = native::Finalize(*generated, options.optimization); !finalized) {
    return diagnostic(finalized.error());
  }
  timings.optimization = Clock::now() - optimizing;
  std::string llvm_ir;
  if (options.preserve_ir) {
    llvm::raw_string_ostream ir_output(llvm_ir);
    module.print(ir_output, nullptr);
  }
  const auto emitting = Clock::now();
  auto object = native::EmitObject(*generated);
  timings.emission = Clock::now() - emitting;
  if (!object) {
    return diagnostic(object.error());
  }
  return NativeArtifact{
      .object = std::move(*object),
      .llvm_ir = std::move(llvm_ir),
      .target = machine.getTargetTriple().str(),
      .exports = std::move(exports),
      .image_symbol = generated->image_symbol,
      .header = std::move(export_interface->header),
      .instantiate_symbol = std::move(export_interface->instantiate_symbol),
      .optimization = options.optimization,
      .program = std::move(program),
      .function_metrics = std::move(generated->function_metrics),
      .timings = timings,
  };
}

}  // namespace vela::compiler::aot
