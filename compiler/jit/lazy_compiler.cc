#include "compiler/jit/lazy_compiler.h"
#include "compiler/jit/code_batch.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/ExecutionEngine/JITSymbol.h>
#include <llvm/ExecutionEngine/Orc/AbsoluteSymbols.h>
#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/CoreContainers.h>
#include <llvm/ExecutionEngine/Orc/IndirectionUtils.h>
#include <llvm/ExecutionEngine/Orc/LazyReexports.h>
#include <llvm/ExecutionEngine/Orc/MaterializationUnit.h>
#include <llvm/ExecutionEngine/Orc/Shared/ExecutorAddress.h>
#include <llvm/ExecutionEngine/Orc/SymbolLookupSet.h>
#include <llvm/ExecutionEngine/Orc/SymbolStringPool.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>

#include "base/execution_status.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/jit/code_inputs.h"
#include "compiler/jit/code_resources.h"
#include "compiler/jit/code_reuse.h"
#include "compiler/jit/compiler.h"
#include "compiler/jit/engine.h"
#include "compiler/jit/function_compiler.h"
#include "compiler/jit/image_ownership.h"
#include "compiler/native/native_image_data.h"
#include "runtime/native/host_callback.h"
#include "runtime/native/native_image.h"

namespace vela::compiler::jit {
namespace {

llvm::Error CompilationError(const std::string& message) {
  return llvm::make_error<llvm::StringError>(message, llvm::inconvertibleErrorCode());
}

// 所有延迟入口（boxed、reflection、typed 与 resume）都以 uint8 状态返回。
// 编译失败时不进入脚本，也不跨越生成代码展开异常；ORC 记录具体诊断。
std::uint8_t CompilationFailed() noexcept {
  return static_cast<std::uint8_t>(ExecutionStatus::NativeFailure);
}

struct LazyBatch final {
  LazyBatch(std::shared_ptr<CodeLinker> source,
            llvm::orc::JITDylib& library,
            std::shared_ptr<CodeResourceCounters> counts,
            std::shared_ptr<FunctionCompiler> compilation)
      : linker(source),
        compiler(std::move(compilation)),
        interned(*source),
        resources(std::move(source), library, std::move(counts), 0) {}

  ~LazyBatch() = default;
  LazyBatch(const LazyBatch&) = delete;
  LazyBatch& operator=(const LazyBatch&) = delete;
  LazyBatch(LazyBatch&&) = delete;
  LazyBatch& operator=(LazyBatch&&) = delete;
  std::expected<std::unique_ptr<llvm::MemoryBuffer>, std::string> Compile(std::size_t index) {
    auto object = compiler->Compile(index);
    if (!object) {
      return std::unexpected(std::move(object.error()));
    }
    resources.functions.fetch_add(1, std::memory_order_relaxed);
    resources.counters->functions.fetch_add(1, std::memory_order_relaxed);
    return std::move(*object);
  }

  // ORC 注册先退休，但其后销毁的跳板与 interned 符号仍借用 ExecutionSession。
  // 编译器可早于代码版本释放；这里保活 linker 直到全部依赖成员销毁。
  std::shared_ptr<CodeLinker> linker;
  std::shared_ptr<FunctionCompiler> compiler;
  BatchSymbols interned;
  std::unique_ptr<llvm::orc::LazyCallThroughManager> call_through;
  std::unique_ptr<llvm::orc::IndirectStubsManager> stubs;
  // 最先销毁 ORC 注册，随后才释放其借用的延迟跳板和编译计划。
  CodeBatch resources;
};

class FunctionMaterialization final : public llvm::orc::MaterializationUnit {
 public:
  FunctionMaterialization(std::weak_ptr<LazyBatch> batch, std::size_t function, Interface symbols)
      : MaterializationUnit(std::move(symbols)), batch_(std::move(batch)), function_(function) {}
  [[nodiscard]] llvm::StringRef getName() const override { return "Vela function"; }

 private:
  void materialize(
      std::unique_ptr<llvm::orc::MaterializationResponsibility> responsibility) override {
    auto batch = batch_.lock();
    auto compiled = batch ? batch->Compile(function_)
                          : std::unexpected(std::string("Vela code owner has retired"));
    if (!compiled) {
      responsibility->getExecutionSession().reportError(CompilationError(compiled.error()));
      responsibility->failMaterialization();
      return;
    }
    batch->resources.jit->getObjLinkingLayer().emit(std::move(responsibility),
                                                    std::move(*compiled));
  }
  void discard(const llvm::orc::JITDylib& /*library*/,
               const llvm::orc::SymbolStringPtr& /*symbol*/) override {}
  // MaterializationUnit 由 JITDylib 拥有，反向弱引用避免 compiler -> JD -> unit -> compiler 环。
  std::weak_ptr<LazyBatch> batch_;
  std::size_t function_;
};

std::expected<void, std::string> DefineAliases(const std::shared_ptr<LazyBatch>& batch,
                                               llvm::orc::SymbolAliasMap aliases,
                                               Materialization materialization) {
  std::unique_ptr<llvm::orc::MaterializationUnit> exports;
  if (materialization == Materialization::OnDemand) {
    exports = llvm::orc::lazyReexports(*batch->call_through, *batch->stubs, batch->resources.dylib,
                                       std::move(aliases));
  } else {
    // 先发布固定入口地址，消除目标文件之间的递归物化依赖；候选尚未交给运行时。
    llvm::orc::IndirectStubsManager::StubInitsMap initial;
    for (const auto& [symbol, target] : aliases) {
      initial[*symbol] = {llvm::orc::ExecutorAddr(), target.AliasFlags};
    }
    if (auto error = batch->stubs->createStubs(initial)) {
      return std::unexpected(llvm::toString(std::move(error)));
    }
    llvm::orc::SymbolMap entries;
    for (const auto& [symbol, target] : aliases) {
      entries.try_emplace(symbol, batch->stubs->findStub(*symbol, false));
    }
    exports = llvm::orc::absoluteSymbols(std::move(entries));
  }
  if (auto error = batch->resources.dylib.define(std::move(exports), batch->resources.tracker)) {
    return std::unexpected(llvm::toString(std::move(error)));
  }
  return {};
}

std::expected<void, std::string> DefineFunctions(const std::shared_ptr<LazyBatch>& batch,
                                                 CodeReusePlan& reuse,
                                                 Materialization materialization) {
  if (materialization == Materialization::OnDemand) {
    auto callbacks = llvm::orc::createLocalLazyCallThroughManager(
        batch->resources.jit->getTargetTriple(), batch->resources.jit->getExecutionSession(),
        llvm::orc::ExecutorAddr::fromPtr(&CompilationFailed));
    if (!callbacks) {
      return std::unexpected(llvm::toString(callbacks.takeError()));
    }
    batch->call_through = std::move(*callbacks);
  }
  auto make_stubs =
      llvm::orc::createLocalIndirectStubsManagerBuilder(batch->resources.jit->getTargetTriple());
  batch->stubs = make_stubs();
  llvm::orc::SymbolAliasMap aliases;
  llvm::orc::SymbolMap retained;
  const auto flags = llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable;
  for (std::size_t index = 0; index < batch->compiler->symbols.size(); ++index) {
    llvm::orc::SymbolFlagsMap implementations;
    const auto first = reuse.functions.size();
    const auto& names = batch->compiler->symbols[index].implementations;
    PlanFunctionBindings(names, index, kFunctionEntrySuffix, reuse);
    if (reuse.reused[index] != 0) {
      for (auto& function : std::span(reuse.functions).subspan(first)) {
        function.symbol = std::string(function.name).append(kFunctionEntrySuffix);
        retained.try_emplace(batch->interned.Get(function.symbol),
                             llvm::orc::ExecutorAddr(function.address), flags);
      }
      continue;
    }
    for (const auto& name : names) {
      auto entry = std::string(name).append(kFunctionEntrySuffix);
      auto entry_symbol = batch->interned.Get(entry);
      auto implementation_symbol = batch->interned.Get(name);
      aliases.try_emplace(entry_symbol, implementation_symbol, flags);
      implementations.try_emplace(implementation_symbol, flags);
    }
    if (auto error = batch->resources.dylib.define(
            std::make_unique<FunctionMaterialization>(
                batch, index,
                llvm::orc::MaterializationUnit::Interface{std::move(implementations), nullptr}),
            batch->resources.tracker)) {
      return std::unexpected(llvm::toString(std::move(error)));
    }
  }
  if (!retained.empty()) {
    if (auto error = batch->resources.dylib.define(llvm::orc::absoluteSymbols(std::move(retained)),
                                                   batch->resources.tracker)) {
      return std::unexpected(llvm::toString(std::move(error)));
    }
  }
  return aliases.empty() ? std::expected<void, std::string>{}
                         : DefineAliases(batch, std::move(aliases), materialization);
}

std::expected<void, std::string> MaterializeFunctions(const std::shared_ptr<LazyBatch>& batch,
                                                      const CodeReusePlan& plan) {
  // ORC 的本地 dispatcher 内联执行物化任务；逐函数等待，避免大批就绪通知递归耗尽栈。
  const auto& symbols = batch->compiler->symbols;
  for (std::size_t index = 0; index < symbols.size(); ++index) {
    if (plan.reused[index] != 0) {
      continue;
    }
    auto entry = batch->resources.jit->getExecutionSession().lookup(
        {&batch->resources.dylib}, batch->interned.Get(symbols[index].external));
    if (!entry) {
      return std::unexpected(llvm::toString(entry.takeError()));
    }
  }
  return {};
}

std::expected<void, std::string> BindEntries(const std::shared_ptr<LazyBatch>& batch,
                                             CodeReusePlan& plan,
                                             native::NativeImageData& data,
                                             ImageOwnership& owner,
                                             CodeReuseCache* reuse,
                                             Materialization materialization) {
  llvm::orc::SymbolLookupSet requested;
  for (const auto& function : plan.functions) {
    if (!function.resource) {
      requested.add(batch->interned.Get(function.symbol));
      if (materialization == Materialization::Eager) {
        requested.add(batch->interned.Get(function.name));
      }
    }
  }
  auto resolved = batch->resources.jit->getExecutionSession().lookup(
      {{&batch->resources.dylib, llvm::orc::JITDylibLookupFlags::MatchExportedSymbolsOnly}},
      requested);
  if (!resolved) {
    return std::unexpected(llvm::toString(resolved.takeError()));
  }
  if (materialization == Materialization::Eager) {
    for (const auto& function : plan.functions) {
      if (!function.resource) {
        const auto symbol = batch->interned.Get(function.symbol);
        const auto address = resolved->at(batch->interned.Get(function.name)).getAddress();
        if (auto error = batch->stubs->updatePointer(*symbol, address)) {
          return std::unexpected(llvm::toString(std::move(error)));
        }
      }
    }
  }
  auto updated = RetainImageCode(batch->interned, *resolved, batch, plan, owner);
  std::vector<runtime::NativeEntry> external;
  std::vector<runtime::NativeReflectionEntry> reflection;
  std::vector<runtime::NativeTypedEntry> typed;
  std::vector<runtime::NativeIntegerCallback> callbacks(batch->compiler->symbols.size());
  for (const auto& symbol : batch->compiler->symbols) {
    external.push_back(
        llvm::orc::ExecutorAddr(updated.at(symbol.external).address).toPtr<runtime::NativeEntry>());
    reflection.push_back(llvm::orc::ExecutorAddr(updated.at(symbol.reflection).address)
                             .toPtr<runtime::NativeReflectionEntry>());
    typed.push_back(llvm::orc::ExecutorAddr(updated.at(symbol.typed).address)
                        .toPtr<runtime::NativeTypedEntry>());
  }
  data.BindEntries(external, reflection, typed, callbacks);
  if (reuse != nullptr) {
    UpdateSourceCache(*reuse, plan, *data.image(), [&](std::size_t index) {
      CodeReuseCache::Source source;
      source.symbols = batch->compiler->symbols[index].implementations;
      source.body_dependencies = plan.inputs[index].dependencies;
      return source;
    });
    reuse->functions = std::move(updated);
  }
  return {};
}

}  // namespace

std::expected<CompiledImage, frontend::Diagnostics> LinkFunctions(
    const std::shared_ptr<CodeLinker>& jit,
    const std::shared_ptr<CodeResourceCounters>& counters,
    std::uint64_t batch_number,
    const std::shared_ptr<native::NativeImageData>& data,
    const CompileOptions& options,
    CodeReuseCache* reuse,
    std::shared_ptr<FunctionCompiler> functions) {
  const auto failure = [](std::string message) {
    return std::unexpected(CompilationDiagnostic(std::move(message)));
  };
  const auto count = data->image()->functions.size;
  const auto started = std::chrono::steady_clock::now();
  auto inputs = std::move(functions->inputs);
  std::unique_lock<std::mutex> cache_lock;
  CodeReusePlan plan;
  if (reuse != nullptr) {
    cache_lock = std::unique_lock(reuse->mutex);
    plan = PlanCodeReuse(std::move(inputs), *reuse);
  } else {
    plan.inputs = std::move(inputs);
    plan.reused.resize(count);
  }
  auto library = jit->createJITDylib("vela_lazy_batch_" + std::to_string(batch_number));
  if (!library) {
    return failure(llvm::toString(library.takeError()));
  }
  library->addToLinkOrder(jit->getMainJITDylib());
  auto batch = std::make_shared<LazyBatch>(jit, *library, counters, std::move(functions));
  if (const auto defined = DefineFunctions(batch, plan, options.materialization); !defined) {
    return failure(defined.error());
  }
  if (options.materialization == Materialization::Eager) {
    if (const auto ready = MaterializeFunctions(batch, plan); !ready) {
      return failure(ready.error());
    }
  }
  auto owner = std::make_shared<ImageOwnership>(counters, *data->image(), data);
  owner->batch = batch;
  if (const auto bound = BindEntries(batch, plan, *data, *owner, reuse, options.materialization);
      !bound) {
    return failure(bound.error());
  }
  CompiledImage result;
  result.image = std::shared_ptr<const runtime::NativeImage>(std::move(owner), data->image());
  result.function_metrics.resize(count);
  result.compiled_functions = plan.compiled_functions;
  result.timings.generation = std::chrono::steady_clock::now() - started;
  if (options.materialization == Materialization::Eager) {
    // 全部目标文件已链接，不再为运行中的版本保留服务端代码生成计划。
    batch->compiler.reset();
  }
  return result;
}

}  // namespace vela::compiler::jit
