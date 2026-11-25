#include "compiler/jit/compiler.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/jit/code_batch.h"
#include "compiler/native/compile_options.h"
#include "runtime/native/host_callback.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <llvm/ADT/StringRef.h>
#include <llvm/ExecutionEngine/JITSymbol.h>
#include <llvm/ExecutionEngine/Orc/AbsoluteSymbols.h>
#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/CoreContainers.h>
#include <llvm/ExecutionEngine/Orc/Shared/ExecutorAddress.h>
#include <llvm/ExecutionEngine/Orc/SymbolLookupSet.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Error.h>
#include "compiler/development/development_files.h"
#include "compiler/development/source_session.h"

#include "compiler/native/field_access.h"
#ifndef NDEBUG
#include "base/native_program_mode.h"
#endif
#include "compiler/frontend/diagnostic.h"
#include "compiler/ir/ir.h"
#include "compiler/jit/code_inputs.h"
#include "compiler/jit/code_resources.h"
#include "compiler/jit/code_reuse.h"
#include "compiler/jit/engine.h"
#include "compiler/jit/image_ownership.h"
#include "compiler/jit/lazy_compiler.h"
#include "compiler/jit/object_compilation.h"
#include "compiler/native/call_targets.h"
#include "compiler/native/code_generator.h"
#include "compiler/native/native_image_data.h"
#include "runtime/native/native_image.h"

namespace vela::compiler::jit {
class LocalCompiler final : public Compiler {
 public:
  explicit LocalCompiler(std::shared_ptr<CodeLinker> jit);
  std::expected<CompiledImage, frontend::Diagnostics> Compile(const ir::Program& program,
                                                              const CompileOptions& options,
                                                              CodeReuseCache* reuse) override;
  std::expected<CompiledImage, frontend::Diagnostics> Compile(
      std::shared_ptr<const ir::Program> program,
      const CompileOptions& options,
      CodeReuseCache* reuse) override;
  CodeResourceCounts resource_counts() const noexcept override;
  std::expected<std::shared_ptr<SourceSession>, frontend::Diagnostics> CreateSources(
      const SourceSessionConfiguration& configuration) override {
    return std::make_shared<SourceSession>(configuration);
  }
  std::expected<std::unique_ptr<DevelopmentFiles>, frontend::Diagnostics> CreateFiles(
      const std::string& entry,
      const frontend::ModulePaths& imports) override {
    return std::make_unique<DevelopmentFiles>(entry, imports);
  }

 private:
  std::shared_ptr<CodeLinker> jit_;
  std::shared_ptr<CodeResourceCounters> counters_;
  std::atomic_uint64_t next_module_{0};
};
namespace {

void ApplyCodeReuse(native::NativeModule& generated,
                    const CodeReuseCache& reuse,
                    std::uint64_t batch,
                    CodeReusePlan& reuse_plan) {
  for (std::size_t index = 0; index < reuse_plan.reused.size(); ++index) {
    if (reuse_plan.reused[index] != 0) {
      const auto& source = reuse.sources[index];
      if (generated.function_symbols[index].empty()) {
        generated.function_symbols[index] = source.symbols;
      }
      generated.function_metrics[index] = source.metrics;
      generated.body_dependencies[index] = source.body_dependencies;
    }
  }
  BindReusableCode(*generated.module, generated.function_symbols, batch, reuse_plan);
}

struct ImageEntrySymbols final {
  std::string boxed;
  std::string reflection;
  std::string typed;
  std::string integer_callback;
};

std::vector<ImageEntrySymbols> EntrySymbols(const native::NativeModule& generated,
                                            const CodeReusePlan& reuse_plan) {
  std::vector<ImageEntrySymbols> entry_symbols;
  for (std::size_t index = 0; index < generated.entry_functions.size(); ++index) {
    const auto* entry = generated.entry_functions[index];
    const auto reused = !reuse_plan.reused.empty() && reuse_plan.reused[index] != 0;
    const auto* callback = generated.integer_callbacks[index];
    entry_symbols.push_back({
        .boxed = reused ? std::string() : entry->getName().str(),
        .reflection =
            reused ? std::string() : generated.reflection_functions[index]->getName().str(),
        .typed = reused ? std::string() : generated.typed_functions[index]->getName().str(),
        .integer_callback =
            reused || callback == nullptr ? std::string() : callback->getName().str(),
    });
  }
  return entry_symbols;
}

llvm::Error DefineReusedSymbols(BatchSymbols& interned,
                                llvm::orc::JITDylib& library,
                                const native::NativeModule& generated,
                                const CodeReusePlan& reuse_plan) {
  llvm::orc::SymbolMap symbols;
  for (const auto& function : reuse_plan.functions) {
    if (function.resource && generated.module->getFunction(function.symbol) != nullptr) {
      symbols.try_emplace(interned.Get(function.symbol), llvm::orc::ExecutorAddr(function.address),
                          llvm::JITSymbolFlags::Exported);
    }
  }
  if (symbols.empty()) {
    return llvm::Error::success();
  }
  return library.define(llvm::orc::absoluteSymbols(std::move(symbols)));
}

llvm::Expected<llvm::orc::SymbolMap> LookupImageSymbols(
    CodeLinker& jit,
    BatchSymbols& interned,
    llvm::orc::JITDylib& library,
    const native::NativeModule& generated,
    const std::vector<ImageEntrySymbols>& entry_symbols,
    const CodeReusePlan& reuse_plan,
    const native::NativeImageData* image_data) {
  llvm::orc::SymbolNameSet requested;
  for (const auto& symbol : entry_symbols) {
    if (!symbol.boxed.empty()) {
      requested.insert(interned.Get(symbol.boxed));
      requested.insert(interned.Get(symbol.reflection));
      requested.insert(interned.Get(symbol.typed));
      if (!symbol.integer_callback.empty()) {
        requested.insert(interned.Get(symbol.integer_callback));
      }
    }
  }
  if (image_data == nullptr) {
    requested.insert(interned.Get(generated.image_symbol));
  }
  for (const auto& function : reuse_plan.functions) {
    if (!function.resource) {
      requested.insert(interned.Get(function.symbol));
    }
  }
  // 一个批次只发起一次链接查询；逐入口等待会重复准备平台与符号依赖。
  return requested.empty()
             ? llvm::Expected<llvm::orc::SymbolMap>(llvm::orc::SymbolMap{})
             : jit.getExecutionSession().lookup(
                   {{&library, llvm::orc::JITDylibLookupFlags::MatchExportedSymbolsOnly}},
                   llvm::orc::SymbolLookupSet(requested));
}

const runtime::NativeImage* BindImageEntries(BatchSymbols& interned,
                                             native::NativeImageData& image_data,
                                             const std::vector<ImageEntrySymbols>& entry_symbols,
                                             const llvm::orc::SymbolMap& resolved,
                                             const CodeReuseCache* reuse) {
  std::vector<runtime::NativeEntry> addresses;
  std::vector<runtime::NativeReflectionEntry> reflection_addresses;
  std::vector<runtime::NativeTypedEntry> typed_addresses;
  std::vector<runtime::NativeIntegerCallback> callbacks;
  for (std::size_t index = 0; index < entry_symbols.size(); ++index) {
    const auto& symbol = entry_symbols[index];
    addresses.push_back(symbol.boxed.empty() ? reuse->sources[index].entry
                                             : resolved.find(interned.Get(symbol.boxed))
                                                   ->second.getAddress()
                                                   .toPtr<runtime::NativeEntry>());
    reflection_addresses.push_back(symbol.reflection.empty()
                                       ? reuse->sources[index].reflection_entry
                                       : resolved.find(interned.Get(symbol.reflection))
                                             ->second.getAddress()
                                             .toPtr<runtime::NativeReflectionEntry>());
    typed_addresses.push_back(symbol.typed.empty() ? reuse->sources[index].typed_entry
                                                   : resolved.find(interned.Get(symbol.typed))
                                                         ->second.getAddress()
                                                         .toPtr<runtime::NativeTypedEntry>());
    runtime::NativeIntegerCallback callback = nullptr;
    if (symbol.boxed.empty()) {
      callback = reuse->sources[index].integer_callback;
    } else if (!symbol.integer_callback.empty()) {
      callback = resolved.find(interned.Get(symbol.integer_callback))
                     ->second.getAddress()
                     .toPtr<runtime::NativeIntegerCallback>();
    }
    callbacks.push_back(callback);
  }
  image_data.BindEntries(addresses, reflection_addresses, typed_addresses, callbacks);
  return image_data.image();
}

}  // namespace

LocalCompiler::LocalCompiler(std::shared_ptr<CodeLinker> jit)
    : jit_(std::move(jit)), counters_(std::make_shared<CodeResourceCounters>()) {
  InstallResourceAccounting(*jit_, counters_);
}

std::expected<std::unique_ptr<Compiler>, frontend::Diagnostics> Compiler::Create() {
  auto created = CreateEngine();
  if (!created) {
    return std::unexpected(CompilationDiagnostic(llvm::toString(created.takeError())));
  }
  return std::make_unique<LocalCompiler>(std::move(*created));
}

CodeResourceCounts LocalCompiler::resource_counts() const noexcept {
  return counters_->Snapshot();
}

std::expected<CompiledImage, frontend::Diagnostics> LocalCompiler::Compile(
    const ir::Program& program,
    const CompileOptions& options,
    CodeReuseCache* reuse) {
  if (options.materialization == Materialization::OnDemand) {
    return std::unexpected(
        CompilationDiagnostic("On-demand compilation requires shared ownership of the program"));
  }
  using Clock = std::chrono::steady_clock;
  CompileTimings timings;
  const auto generation_started = Clock::now();
  const auto batch = next_module_.fetch_add(1);
  std::unique_lock<std::mutex> reuse_lock;
  CodeReusePlan reuse_plan;
  const auto calls = native::PlanNativeCalls(program, options.tier, options.debugging);
  const auto fields = native::FindFieldTargets(program);
  auto compiled_functions = program.functions.size();
  if (reuse != nullptr) {
    assert(program.native_mode == NativeProgramMode::Reloadable);
    reuse_lock = std::unique_lock(reuse->mutex);
    reuse_plan = PlanCodeReuse(DescribeCodeInputs(program, calls, fields, options.tier,
                                                  static_cast<std::uint8_t>(options.optimization),
                                                  static_cast<std::uint8_t>(options.debugging),
                                                  static_cast<std::uint8_t>(options.escape)),
                               *reuse);
  }
  const std::string module_name = "vela_jit";
  auto generated = native::Generate(program, calls, fields, module_name, options.debugging,
                                    options.tier, options.escape, NativeCodePlacement::Dynamic,
                                    options.optimization, reuse_plan.reused);
  timings.generation = Clock::now() - generation_started;
  if (!generated) {
    return std::unexpected(std::move(generated.error()));
  }
  assert(generated->module->getTargetTriple() == jit_->getTargetTriple());
  assert(generated->module->getDataLayout() == jit_->getDataLayout());
  if (reuse != nullptr) {
    const auto planning = Clock::now();
    ApplyCodeReuse(*generated, *reuse, batch, reuse_plan);
    compiled_functions = reuse_plan.compiled_functions;
    timings.generation += Clock::now() - planning;
  }
  auto image_data = std::move(generated->image_data);
  const auto entry_symbols = EntrySymbols(*generated, reuse_plan);
  auto prepared = PrepareObject(*generated, options, timings);
  if (!prepared) {
    return std::unexpected(CompilationDiagnostic(prepared.error()));
  }
  auto& cached = prepared->cached;
  const auto linking_started = Clock::now();
  auto library = jit_->createJITDylib("vela_batch_" + std::to_string(batch));
  if (!library) {
    return std::unexpected(CompilationDiagnostic(llvm::toString(library.takeError())));
  }
  library->addToLinkOrder(jit_->getMainJITDylib());
  auto resource = std::make_shared<CodeBatch>(jit_, *library, counters_, compiled_functions);
  BatchSymbols interned(*jit_);
  if (auto error = DefineReusedSymbols(interned, *library, *generated, reuse_plan)) {
    return std::unexpected(CompilationDiagnostic(llvm::toString(std::move(error))));
  }
  if (auto error = jit_->addObjectFile(resource->tracker, std::move(cached.object))) {
    return std::unexpected(CompilationDiagnostic(llvm::toString(std::move(error))));
  }
  auto resolved = LookupImageSymbols(*jit_, interned, *library, *generated, entry_symbols,
                                     reuse_plan, image_data.get());
  if (!resolved) {
    return std::unexpected(CompilationDiagnostic(llvm::toString(resolved.takeError())));
  }
  const runtime::NativeImage* image = nullptr;
  if (image_data) {
    image = BindImageEntries(interned, *image_data, entry_symbols, *resolved, reuse);
  } else {
    const auto entry = resolved->find(interned.Get(generated->image_symbol))->second.getAddress();
    using ImageEntry = const runtime::NativeImage* (*)();
    image = entry.toPtr<ImageEntry>()();
  }
  auto owner = std::make_shared<ImageOwnership>(counters_, *image, std::move(image_data));
  owner->batch = resource;
  auto updated = RetainImageCode(interned, *resolved, resource, reuse_plan, *owner);
  timings.linking = Clock::now() - linking_started;
  if (reuse != nullptr) {
    reuse->functions = std::move(updated);
    UpdateSourceCache(*reuse, reuse_plan, *image, [&](std::size_t index) {
      CodeReuseCache::Source source;
      source.symbols = std::move(generated->function_symbols[index]);
      source.metrics = generated->function_metrics[index];
      source.body_dependencies = std::move(generated->body_dependencies[index]);
      return source;
    });
  }
  return CompiledImage{
      .image = std::shared_ptr<const runtime::NativeImage>(std::move(owner), image),
      .llvm_ir = std::move(prepared->llvm_ir),
      .function_metrics = std::move(generated->function_metrics),
      .cache = cached.status,
      .timings = timings,
      .compiled_functions = compiled_functions,
  };
}

std::expected<CompiledImage, frontend::Diagnostics> LocalCompiler::Compile(
    std::shared_ptr<const ir::Program> program,
    const CompileOptions& options,
    CodeReuseCache* reuse) {
  if (options.materialization == Materialization::Eager) {
    return Compile(*program, options, reuse);
  }
  return CompileOnDemand(jit_, counters_, next_module_.fetch_add(1), program, options, reuse);
}

}  // namespace vela::compiler::jit
