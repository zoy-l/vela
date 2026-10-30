#include "compiler/jit/compiler.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/Layer.h>
#include <llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Triple.h>

#include "base/declaration.h"
#include "base/native_program_mode.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/ir/ir.h"
#include "compiler/jit/code_cache.h"
#include "compiler/jit/code_resources.h"
#include "compiler/jit/platform_support.h"
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
    counters->constant_payload_bytes.fetch_sub(constant_payload_bytes, std::memory_order_relaxed);
    counters->debug_payload_bytes.fetch_sub(debug_payload_bytes, std::memory_order_relaxed);
  }
  void AccountImage(const runtime::NativeImage& image) noexcept {
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
  std::size_t constant_payload_bytes = 0;
  std::size_t debug_payload_bytes = 0;
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
    const auto object = native::EmitObject(generated);
    if (!object) {
      return std::unexpected(object.error());
    }
    const void* storage = object->data();
    cached.object = llvm::MemoryBuffer::getMemBufferCopy(
        {static_cast<const char*>(storage), object->size()}, generated.module->getName());
    timings.compilation = Clock::now() - started;
  }
  return result;
}

}  // namespace

Compiler::Compiler(std::shared_ptr<llvm::orc::LLJIT> jit)
    : jit_(std::move(jit)), counters_(std::make_shared<CodeResourceCounters>()) {
  InstallResourceAccounting(*jit_, counters_);
}

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
        return std::make_unique<llvm::orc::ObjectLinkingLayer>(session);
      });
  auto created = builder.create();
  if (!created) {
    return std::unexpected(Diagnostic(llvm::toString(created.takeError())));
  }
  if (auto error = DefineRuntimeSymbols(**created)) {
    return std::unexpected(Diagnostic(llvm::toString(std::move(error))));
  }
  if (auto error = InstallPlatformSupport(**created)) {
    return std::unexpected(Diagnostic(llvm::toString(std::move(error))));
  }
  return std::unique_ptr<Compiler>(new Compiler(std::move(*created)));
}

CodeResourceCounts Compiler::resource_counts() const noexcept {
  return {
      .batches = counters_->batches.load(std::memory_order_relaxed),
      .functions = counters_->functions.load(std::memory_order_relaxed),
      .executable_bytes = counters_->executable_bytes.load(std::memory_order_relaxed),
      .read_only_bytes = counters_->read_only_bytes.load(std::memory_order_relaxed),
      .writable_bytes = counters_->writable_bytes.load(std::memory_order_relaxed),
      .constant_payload_bytes = counters_->constant_payload_bytes.load(std::memory_order_relaxed),
      .debug_payload_bytes = counters_->debug_payload_bytes.load(std::memory_order_relaxed),
  };
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
  using Clock = std::chrono::steady_clock;
  CompileTimings timings;
  const auto generation_started = Clock::now();
  const std::string module_name = "vela_jit";
  auto generated = native::Generate(program, module_name, exports, options.debugging, options.tier,
                                    options.escape, NativeCodePlacement::Dynamic);
  timings.generation = Clock::now() - generation_started;
  if (!generated) {
    return std::unexpected(std::move(generated.error()));
  }
  if (generated->module->getTargetTriple() != jit_->getTargetTriple() ||
      generated->module->getDataLayout() != jit_->getDataLayout()) {
    return std::unexpected(Diagnostic("JIT target does not match the native runtime ABI"));
  }
  auto prepared = PrepareObject(*generated, options, timings);
  if (!prepared) {
    return std::unexpected(Diagnostic(prepared.error()));
  }
  auto& cached = prepared->cached;
  const auto linking_started = Clock::now();
  auto library = jit_->createJITDylib("vela_batch_" + std::to_string(next_module_.fetch_add(1)));
  if (!library) {
    return std::unexpected(Diagnostic(llvm::toString(library.takeError())));
  }
  library->addToLinkOrder(jit_->getMainJITDylib());
  auto resource = std::make_shared<CodeBatch>(jit_, *library, counters_, program.functions.size());
  const auto save_cache = options.code_cache && cached.status != CodeCacheStatus::Hit;
  auto object = save_cache ? llvm::MemoryBuffer::getMemBufferCopy(cached.object->getBuffer())
                           : std::move(cached.object);
  if (auto error = jit_->addObjectFile(resource->tracker, std::move(object))) {
    return std::unexpected(Diagnostic(llvm::toString(std::move(error))));
  }
  auto entry = jit_->lookup(*library, generated->image_symbol);
  if (!entry) {
    return std::unexpected(Diagnostic(llvm::toString(entry.takeError())));
  }
  using ImageEntry = const runtime::NativeImage* (*)();
  const auto* image = entry->toPtr<ImageEntry>()();
  resource->AccountImage(*image);
  timings.linking = Clock::now() - linking_started;
  if (save_cache) {
    const auto started = Clock::now();
    if (const auto saved =
            WriteCodeCache(*options.code_cache, prepared->key, cached.object->getMemBufferRef());
        !saved) {
      return std::unexpected(Diagnostic(saved.error()));
    }
    timings.cache += Clock::now() - started;
  }
  return CompiledImage{
      .image = std::shared_ptr<const runtime::NativeImage>(std::move(resource), image),
      .llvm_ir = std::move(prepared->llvm_ir),
      .function_metrics = std::move(generated->function_metrics),
      .cache = cached.status,
      .timings = timings,
  };
}

}  // namespace vela::compiler::jit
