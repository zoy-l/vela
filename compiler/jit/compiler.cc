#include "compiler/jit/compiler.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include <llvm/ADT/StringRef.h>
#include <llvm/ExecutionEngine/JITSymbol.h>
#include <llvm/ExecutionEngine/Orc/AbsoluteSymbols.h>
#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/CoreContainers.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/Shared/ExecutorAddress.h>
#include <llvm/ExecutionEngine/Orc/SymbolStringPool.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Triple.h>

#include "base/declaration.h"
#ifndef NDEBUG
#include "base/native_program_mode.h"
#endif
#include "compiler/frontend/diagnostic.h"
#include "compiler/ir/ir.h"
#include "compiler/jit/code_cache.h"
#include "compiler/jit/code_inputs.h"
#include "compiler/jit/code_resources.h"
#include "compiler/jit/code_reuse.h"
#include "compiler/jit/engine.h"
#include "compiler/native/call_targets.h"
#include "compiler/native/code_generator.h"
#include "compiler/native/host_header.h"
#include "compiler/native/native_image_data.h"
#include "runtime/native/native_image.h"

namespace vela::compiler::jit {
namespace {

frontend::Diagnostics Diagnostic(std::string message) {
  return {{.location = {}, .message = std::move(message), .source_path = {}}};
}

struct CodeBatch final {
  CodeBatch(std::shared_ptr<llvm::orc::LLJIT> source,
            llvm::orc::JITDylib& library,
            std::shared_ptr<CodeResourceCounters> counts,
            std::size_t count)
      : jit(std::move(source)),
        dylib(&library),
        tracker(library.createResourceTracker()),
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
    if (auto error = jit->getExecutionSession().removeJITDylib(*dylib)) {
      jit->getExecutionSession().reportError(std::move(error));
    }
    counters->functions.fetch_sub(functions, std::memory_order_relaxed);
    counters->batches.fetch_sub(1, std::memory_order_relaxed);
  }
  CodeBatch(const CodeBatch&) = delete;
  CodeBatch& operator=(const CodeBatch&) = delete;
  CodeBatch(CodeBatch&&) = delete;
  CodeBatch& operator=(CodeBatch&&) = delete;
  std::shared_ptr<llvm::orc::LLJIT> jit;
  llvm::orc::JITDylib* dylib;
  llvm::orc::ResourceTrackerSP tracker;
  std::shared_ptr<CodeResourceCounters> counters;
  std::size_t functions;
  std::vector<std::shared_ptr<void>> dependencies;
};

struct ImageOwnership final {
  ImageOwnership(std::shared_ptr<CodeResourceCounters> counts,
                 const runtime::NativeImage& image,
                 std::shared_ptr<native::NativeImageData> storage)
      : counters(std::move(counts)), data(std::move(storage)) {
    const auto strings = image.strings.values();
    constant_payload_bytes = strings.size_bytes();
    for (const auto& text : strings) {
      constant_payload_bytes += text.size;
    }
    const auto paths = image.source_paths.values();
    const auto sources = image.source_texts.values();
    debug_payload_bytes = paths.size_bytes() + sources.size_bytes();
    for (const auto& path : paths) {
      debug_payload_bytes += path.size;
    }
    for (const auto& source : sources) {
      debug_payload_bytes += source.size;
    }
    for (const auto& function : image.functions.values()) {
      debug_payload_bytes += function.locations.values().size_bytes() + function.name.size +
                             function.native_symbol.size + sizeof(FunctionIdentity);
    }
    counters->constant_payload_bytes.fetch_add(constant_payload_bytes, std::memory_order_relaxed);
    counters->debug_payload_bytes.fetch_add(debug_payload_bytes, std::memory_order_relaxed);
    counters->images.fetch_add(1, std::memory_order_relaxed);
    image_data_bytes = data ? data->bytes() : 0;
    counters->image_data_bytes.fetch_add(image_data_bytes, std::memory_order_relaxed);
  }
  ~ImageOwnership() {
    counters->images.fetch_sub(1, std::memory_order_relaxed);
    counters->image_data_bytes.fetch_sub(image_data_bytes, std::memory_order_relaxed);
    counters->constant_payload_bytes.fetch_sub(constant_payload_bytes, std::memory_order_relaxed);
    counters->debug_payload_bytes.fetch_sub(debug_payload_bytes, std::memory_order_relaxed);
  }
  ImageOwnership(const ImageOwnership&) = delete;
  ImageOwnership& operator=(const ImageOwnership&) = delete;
  ImageOwnership(ImageOwnership&&) = delete;
  ImageOwnership& operator=(ImageOwnership&&) = delete;
  std::shared_ptr<CodeResourceCounters> counters;
  std::size_t image_data_bytes = 0;
  std::size_t constant_payload_bytes = 0;
  std::size_t debug_payload_bytes = 0;
  std::shared_ptr<native::NativeImageData> data;
  std::shared_ptr<CodeBatch> batch;
  std::vector<std::shared_ptr<void>> code;
};

struct CompiledObject final {
  CachedObject cached;
  std::string key;
  std::string llvm_ir;
};

std::expected<CompiledObject, std::string> PrepareObject(native::NativeModule& generated,
                                                         const CompileOptions& options,
                                                         CompileTimings& timings) {
  using Clock = std::chrono::steady_clock;
  CompiledObject result{
      .cached = {.status = CodeCacheStatus::Disabled, .object = {}},
      .key = {},
      .llvm_ir = {},
  };
  auto& cached = result.cached;
  if (options.code_cache) {
    const auto started = Clock::now();
    result.key = CodeCacheKey(*generated.module, options.optimization);
    auto loaded = ReadCodeCache(*options.code_cache, result.key);
    if (!loaded) {
      return std::unexpected(loaded.error());
    }
    cached = std::move(*loaded);
    timings.cache = Clock::now() - started;
  }
  if (cached.status != CodeCacheStatus::Hit || options.preserve_ir) {
    const auto started = Clock::now();
    if (const auto finalized = native::Finalize(generated, options.optimization); !finalized) {
      return std::unexpected(finalized.error());
    }
    timings.optimization = Clock::now() - started;
  }
  if (options.preserve_ir) {
    llvm::raw_string_ostream output(result.llvm_ir);
    generated.module->print(output, nullptr);
  }
  if (cached.object == nullptr) {
    const auto started = Clock::now();
    auto object = native::EmitObject(generated);
    if (!object) {
      return std::unexpected(object.error());
    }
    cached.object = std::move(*object);
    timings.compilation = Clock::now() - started;
  }
  return result;
}

std::vector<std::uint8_t> ExportedFunctions(const ir::Program& program) {
  assert(program.native_mode != NativeProgramMode::Reloadable || program.revision != 0);
  std::vector<std::uint8_t> exports;
  exports.reserve(program.functions.size());
  for (const auto& function : program.functions) {
    exports.push_back(function.is_exported ? 1 : 0);
  }
  return exports;
}

void ApplyCodeReuse(native::NativeModule& generated,
                    const CodeReuseCache& reuse,
                    std::uint64_t batch,
                    CodeReusePlan& reuse_plan) {
  for (std::size_t index = 0; index < reuse_plan.reused.size(); ++index) {
    if (reuse_plan.reused.at(index) != 0 && generated.function_symbols.at(index).empty()) {
      generated.function_symbols.at(index) = reuse.sources.at(index).symbols;
    }
  }
  BindReusableCode(*generated.module, reuse, generated.function_symbols, batch, reuse_plan);
  for (std::size_t index = 0; index < reuse_plan.reused.size(); ++index) {
    if (reuse_plan.reused.at(index) != 0) {
      const auto& source = reuse.sources.at(index);
      generated.function_metrics.at(index) = source.metrics;
      generated.body_dependencies.at(index) = source.body_dependencies;
    }
  }
}

// 一批代码的所有查询、复用与入口绑定共用已驻留的符号，避免反复锁住 LLVM 符号池。
class BatchSymbols final {
 public:
  explicit BatchSymbols(llvm::orc::LLJIT& jit) : jit_(&jit) {}
  llvm::orc::SymbolStringPtr Get(std::string_view name) {
    const auto [found, inserted] = symbols_.try_emplace(name);
    if (inserted) {
      found->second = jit_->mangleAndIntern(llvm::StringRef(name.data(), name.size()));
    }
    return found->second;
  }

 private:
  llvm::orc::LLJIT* jit_;
  absl::flat_hash_map<std::string, llvm::orc::SymbolStringPtr> symbols_;
};

struct FunctionEntrySymbols final {
  std::string boxed;
  std::string typed;
};

std::vector<FunctionEntrySymbols> EntrySymbols(const native::NativeModule& generated,
                                               const CodeReusePlan& reuse_plan) {
  std::vector<FunctionEntrySymbols> entry_symbols;
  for (std::size_t index = 0; index < generated.entry_functions.size(); ++index) {
    const auto* entry = generated.entry_functions.at(index);
    const auto reused = !reuse_plan.reused.empty() && reuse_plan.reused.at(index) != 0;
    entry_symbols.push_back({
        .boxed = reused ? std::string() : entry->getName().str(),
        .typed = reused ? std::string() : generated.typed_functions.at(index)->getName().str(),
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
    llvm::orc::LLJIT& jit,
    BatchSymbols& interned,
    llvm::orc::JITDylib& library,
    const native::NativeModule& generated,
    const std::vector<FunctionEntrySymbols>& entry_symbols,
    const CodeReusePlan& reuse_plan,
    const native::NativeImageData* image_data) {
  llvm::orc::SymbolNameSet requested;
  for (const auto& symbol : entry_symbols) {
    if (!symbol.boxed.empty()) {
      requested.insert(interned.Get(symbol.boxed));
      requested.insert(interned.Get(symbol.typed));
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
                                             const std::vector<FunctionEntrySymbols>& entry_symbols,
                                             const llvm::orc::SymbolMap& resolved,
                                             const CodeReuseCache* reuse) {
  std::vector<runtime::NativeEntry> addresses;
  std::vector<runtime::NativeTypedEntry> typed_addresses;
  for (std::size_t index = 0; index < entry_symbols.size(); ++index) {
    const auto& symbol = entry_symbols.at(index);
    addresses.push_back(symbol.boxed.empty() ? reuse->sources.at(index).entry
                                             : resolved.find(interned.Get(symbol.boxed))
                                                   ->second.getAddress()
                                                   .toPtr<runtime::NativeEntry>());
    typed_addresses.push_back(symbol.typed.empty() ? reuse->sources.at(index).typed_entry
                                                   : resolved.find(interned.Get(symbol.typed))
                                                         ->second.getAddress()
                                                         .toPtr<runtime::NativeTypedEntry>());
  }
  image_data.BindEntries(addresses, typed_addresses);
  return image_data.image();
}

absl::flat_hash_map<std::string, ReusableCode> RetainImageCode(
    BatchSymbols& interned,
    const llvm::orc::SymbolMap& resolved,
    const std::shared_ptr<CodeBatch>& resource,
    CodeReusePlan& reuse_plan,
    ImageOwnership& owner) {
  absl::flat_hash_set<const void*> owners;
  absl::flat_hash_map<std::string, ReusableCode> updated;
  for (auto& function : reuse_plan.functions) {
    if (!function.resource) {
      function.address =
          resolved.find(interned.Get(function.symbol))->second.getAddress().getValue();
      function.resource = resource;
    }
    if (owners.insert(function.resource.get()).second) {
      owner.code.push_back(function.resource);
    }
    updated.emplace(std::move(function.name), ReusableCode{
                                                  .symbol = std::move(function.symbol),
                                                  .resource = function.resource,
                                                  .address = function.address,
                                              });
  }
  return updated;
}

void UpdateSourceCache(CodeReuseCache& reuse,
                       CodeReusePlan& reuse_plan,
                       native::NativeModule& generated,
                       const runtime::NativeImage& image) {
  reuse.sources.clear();
  const auto functions = image.functions.values();
  for (std::size_t index = 0; index < reuse_plan.inputs.size(); ++index) {
    const auto& function = functions.subspan(index).front();
    reuse.sources.push_back({
        .fingerprint = std::move(reuse_plan.inputs.at(index).fingerprint),
        .symbols = std::move(generated.function_symbols.at(index)),
        .metrics = generated.function_metrics.at(index),
        .entry = function.entry,
        .typed_entry = function.typed_entry,
        .body_dependencies = std::move(generated.body_dependencies.at(index)),
    });
  }
}

}  // namespace

Compiler::Compiler(std::shared_ptr<llvm::orc::LLJIT> jit)
    : jit_(std::move(jit)), counters_(std::make_shared<CodeResourceCounters>()) {
  InstallResourceAccounting(*jit_, counters_);
}

std::expected<std::unique_ptr<Compiler>, frontend::Diagnostics> Compiler::Create() {
  const auto identity = runtime::NativeTargetIdentity();
  auto created = CreateEngine(llvm::Triple(llvm::StringRef(identity.data(), identity.size())));
  if (!created) {
    return std::unexpected(Diagnostic(llvm::toString(created.takeError())));
  }
  return std::unique_ptr<Compiler>(new Compiler(std::move(*created)));
}

CodeResourceCounts Compiler::resource_counts() const noexcept {
  return {
      .batches = counters_->batches.load(std::memory_order_relaxed),
      .functions = counters_->functions.load(std::memory_order_relaxed),
      .images = counters_->images.load(std::memory_order_relaxed),
      .image_data_bytes = counters_->image_data_bytes.load(std::memory_order_relaxed),
      .executable_bytes = counters_->executable_bytes.load(std::memory_order_relaxed),
      .read_only_bytes = counters_->read_only_bytes.load(std::memory_order_relaxed),
      .writable_bytes = counters_->writable_bytes.load(std::memory_order_relaxed),
      .constant_payload_bytes = counters_->constant_payload_bytes.load(std::memory_order_relaxed),
      .debug_payload_bytes = counters_->debug_payload_bytes.load(std::memory_order_relaxed),
  };
}

std::expected<CompiledImage, frontend::Diagnostics> Compiler::Compile(const ir::Program& program,
                                                                      const CompileOptions& options,
                                                                      CodeReuseCache* reuse) {
  const auto exports = ExportedFunctions(program);
  std::string host_header;
  if (options.host_namespace) {
    auto emitted = native::EmitHostDeclarations(program, *options.host_namespace);
    if (!emitted) {
      return std::unexpected(Diagnostic(emitted.error()));
    }
    host_header = std::move(*emitted);
  }
  using Clock = std::chrono::steady_clock;
  CompileTimings timings;
  const auto generation_started = Clock::now();
  const auto batch = next_module_.fetch_add(1);
  std::unique_lock<std::mutex> reuse_lock;
  CodeReusePlan reuse_plan;
  const auto calls = native::PlanNativeCalls(program, options.tier);
  auto compiled_functions = program.functions.size();
  if (reuse != nullptr) {
    assert(program.native_mode == NativeProgramMode::Reloadable);
    reuse_lock = std::unique_lock(reuse->mutex);
    reuse_plan = PlanCodeReuse(DescribeCodeInputs(program, calls, options.tier,
                                                  static_cast<std::uint8_t>(options.optimization),
                                                  static_cast<std::uint8_t>(options.debugging),
                                                  static_cast<std::uint8_t>(options.escape)),
                               *reuse);
  }
  const std::string module_name = "vela_jit";
  auto generated =
      native::Generate(program, calls, module_name, exports, options.debugging, options.tier,
                       options.escape, NativeCodePlacement::Dynamic, reuse_plan.reused);
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
    return std::unexpected(Diagnostic(prepared.error()));
  }
  auto& cached = prepared->cached;
  if (options.code_cache && cached.status != CodeCacheStatus::Hit) {
    // 已验证的目标文件先写缓存，再把唯一缓冲交给 ORC；I/O 失败时不建立代码批次。
    const auto started = Clock::now();
    if (const auto saved =
            WriteCodeCache(*options.code_cache, prepared->key, cached.object->getMemBufferRef());
        !saved) {
      return std::unexpected(Diagnostic(saved.error()));
    }
    timings.cache += Clock::now() - started;
  }
  const auto linking_started = Clock::now();
  auto library = jit_->createJITDylib("vela_batch_" + std::to_string(batch));
  if (!library) {
    return std::unexpected(Diagnostic(llvm::toString(library.takeError())));
  }
  library->addToLinkOrder(jit_->getMainJITDylib());
  auto resource = std::make_shared<CodeBatch>(jit_, *library, counters_, compiled_functions);
  resource->dependencies = std::move(reuse_plan.dependencies);
  BatchSymbols interned(*jit_);
  if (auto error = DefineReusedSymbols(interned, *library, *generated, reuse_plan)) {
    return std::unexpected(Diagnostic(llvm::toString(std::move(error))));
  }
  if (auto error = jit_->addObjectFile(resource->tracker, std::move(cached.object))) {
    return std::unexpected(Diagnostic(llvm::toString(std::move(error))));
  }
  auto resolved = LookupImageSymbols(*jit_, interned, *library, *generated, entry_symbols,
                                     reuse_plan, image_data.get());
  if (!resolved) {
    return std::unexpected(Diagnostic(llvm::toString(resolved.takeError())));
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
    UpdateSourceCache(*reuse, reuse_plan, *generated, *image);
  }
  return CompiledImage{
      .image = std::shared_ptr<const runtime::NativeImage>(std::move(owner), image),
      .llvm_ir = std::move(prepared->llvm_ir),
      .function_metrics = std::move(generated->function_metrics),
      .cache = cached.status,
      .timings = timings,
      .compiled_functions = compiled_functions,
      .host_header = std::move(host_header),
  };
}

}  // namespace vela::compiler::jit
