#include "compiler/jit/compiler.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/Debugging/DebuggerSupport.h>
#include <llvm/ExecutionEngine/Orc/EHFrameRegistrationPlugin.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Triple.h>

#include "base/declaration.h"
#include "base/native_program_mode.h"
#include "compiler/jit/runtime_symbols.h"
#include "compiler/native/code_generator.h"
#include "runtime/native_image.h"

namespace vela::compiler::jit {
namespace {

frontend::Diagnostics Diagnostic(std::string message) {
  return {{.location = {}, .message = std::move(message), .source_path = {}}};
}

struct CodeBatch final {
  CodeBatch(std::shared_ptr<llvm::orc::LLJIT> source,
            std::shared_ptr<CodeResourceCounters> counts,
            std::size_t count)
      : jit(std::move(source)),
        tracker(jit->getMainJITDylib().createResourceTracker()),
        counters(std::move(counts)),
        functions(count) {
    counters->batches.fetch_add(1, std::memory_order_relaxed);
    counters->functions.fetch_add(functions, std::memory_order_relaxed);
  }
  ~CodeBatch() {
    if (!tracker->isDefunct()) {
      if (auto error = tracker->remove()) {
        jit->getExecutionSession().reportError(std::move(error));
      }
    }
    counters->functions.fetch_sub(functions, std::memory_order_relaxed);
    counters->batches.fetch_sub(1, std::memory_order_relaxed);
  }
  std::shared_ptr<llvm::orc::LLJIT> jit;
  llvm::orc::ResourceTrackerSP tracker;
  std::shared_ptr<CodeResourceCounters> counters;
  std::size_t functions;
};

}  // namespace

Compiler::Compiler(std::shared_ptr<llvm::orc::LLJIT> jit)
    : jit_(std::move(jit)), counters_(std::make_shared<CodeResourceCounters>()) {}

std::expected<std::unique_ptr<Compiler>, frontend::Diagnostics> Compiler::Create() {
  native::InitializeTarget();
  llvm::orc::JITTargetMachineBuilder target{
      llvm::Triple(std::string(runtime::NativeTargetIdentity()))};
  target.setCPU("generic");
  llvm::orc::LLJITBuilder builder;
  builder.setJITTargetMachineBuilder(std::move(target));
  builder.setObjectLinkingLayerCreator(
      [](llvm::orc::ExecutionSession& session)
          -> llvm::Expected<std::unique_ptr<llvm::orc::ObjectLayer>> {
        auto frames = llvm::orc::EHFrameRegistrationPlugin::Create(session);
        if (!frames) {
          return frames.takeError();
        }
        auto layer = std::make_unique<llvm::orc::ObjectLinkingLayer>(session);
        layer->addPlugin(std::move(*frames));
        return layer;
      });
  auto created = builder.create();
  if (!created) {
    return std::unexpected(Diagnostic(llvm::toString(created.takeError())));
  }
  if (auto error = DefineRuntimeSymbols(**created)) {
    return std::unexpected(Diagnostic(llvm::toString(std::move(error))));
  }
  if (auto error = llvm::orc::enableDebuggerSupport(**created)) {
    return std::unexpected(Diagnostic(llvm::toString(std::move(error))));
  }
  return std::unique_ptr<Compiler>(new Compiler(std::move(*created)));
}

CodeResourceCounts Compiler::resource_counts() const noexcept {
  return {.batches = counters_->batches.load(std::memory_order_relaxed),
          .functions = counters_->functions.load(std::memory_order_relaxed)};
}

std::expected<CompiledImage, frontend::Diagnostics> Compiler::Compile(
    const ir::Program& program,
    const CompileOptions& options) {
  if (program.native_mode != NativeProgramMode::Reloadable || program.revision == 0) {
    return std::unexpected(Diagnostic("JIT requires a versioned reloadable source candidate"));
  }
  std::vector<std::uint8_t> exports;
  exports.reserve(program.functions.size());
  for (const auto& function : program.functions) {
    const auto identity = function.identity;
    if (identity.revision != program.revision ||
        (identity.local == 0 && identity.declaration == DeclarationId{})) {
      return std::unexpected(Diagnostic("JIT function is missing its immutable version identity"));
    }
    exports.push_back(function.is_exported ? 1 : 0);
  }
  const auto module_name = "vela_jit_" + std::to_string(next_module_.fetch_add(1));
  auto generated = native::Generate(program, module_name, exports);
  if (!generated) {
    return std::unexpected(std::move(generated.error()));
  }
  if (generated->module->getTargetTriple() != jit_->getTargetTriple() ||
      generated->module->getDataLayout() != jit_->getDataLayout()) {
    return std::unexpected(Diagnostic("JIT target does not match the native runtime ABI"));
  }
  if (const auto finalized = native::Finalize(*generated, options.optimization); !finalized) {
    return std::unexpected(Diagnostic(finalized.error()));
  }
  std::string llvm_ir;
  if (options.preserve_ir) {
    llvm::raw_string_ostream output(llvm_ir);
    generated->module->print(output, nullptr);
  }
  auto resource = std::make_shared<CodeBatch>(jit_, counters_, program.functions.size());
  llvm::orc::ThreadSafeModule module(std::move(generated->module), std::move(generated->context));
  if (auto error = jit_->addIRModule(resource->tracker, std::move(module))) {
    return std::unexpected(Diagnostic(llvm::toString(std::move(error))));
  }
  auto entry = jit_->lookup(generated->image_symbol);
  if (!entry) {
    return std::unexpected(Diagnostic(llvm::toString(entry.takeError())));
  }
  using ImageEntry = const runtime::NativeImage* (*)();
  const auto* image = entry->toPtr<ImageEntry>()();
  return CompiledImage{
      .image = std::shared_ptr<const runtime::NativeImage>(std::move(resource), image),
      .llvm_ir = std::move(llvm_ir),
      .function_metrics = std::move(generated->function_metrics),
  };
}

}  // namespace vela::compiler::jit
