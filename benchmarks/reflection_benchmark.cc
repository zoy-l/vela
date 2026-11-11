#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>
#include <vela/host.h>

#include "base/native_program_mode.h"
#include "benchmarks/heap_metrics.h"
#include "benchmarks/jit_benchmark.h"
#include "benchmarks/reflection_benchmark.h"
#include "benchmarks/reflection_native/entries.h"
#include "compiler/development/development_session.h"
#include "compiler/development/source_session.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/jit/code_resources.h"
#include "compiler/jit/compiler.h"
#include "compiler/jit/source_compiler.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/handle_access.h"
#include "runtime/native/native_dispatch.h"
#include "runtime/native/native_execution.h"
#include "runtime/native/native_image.h"
#include "runtime/native/native_instance.h"
#include "runtime/native/native_program_state.h"

namespace vela::benchmarks {
namespace {

using reflection_benchmark::Program;
using reflection_benchmark::Registration;
using reflection_benchmark::ScriptRun;
using reflection_benchmark::Source;

struct CompiledProgram final {
  std::shared_ptr<const runtime::NativeImage> image;
  compiler::jit::CodeResourceCounts resources;
};

struct ScriptMode final {
  std::string_view name;
  NativeProgramMode program;
  NativeCodeTier tier;
};

class ScriptProgram final {
 public:
  ScriptProgram(ScriptMode mode, std::span<const Source> sources)
      : mode_(mode), sources_(sources) {}

  std::expected<const CompiledProgram*, std::string> Prepare() {
    // 只复用不可变映像；每个样本仍创建新 Isolate 与堆。
    if (!compiled_) {
      compiled_ = Compile();
    }
    const auto& result = *compiled_;
    if (!result) {
      return std::unexpected(result.error());
    }
    return &*result;
  }

 private:
  std::expected<compiler::jit::CompiledImage, std::string> CompileImage(
      compiler::jit::Compiler& compiler) const {
    compiler::jit::SourceOptions options;
    options.compilation.tier = mode_.tier;
    if (mode_.program == NativeProgramMode::Reloadable) {
      compiler::SourceSession session(std::string(sources_.front().path));
      compiler::SourceUpdate update{.changes = {}, .added_roots = {}};
      for (const auto& source : sources_) {
        update.changes.push_back(
            {.path = std::string(source.path), .text = std::string(source.text)});
      }
      const auto candidate = session.Prepare(std::move(update));
      if (!candidate) {
        return std::unexpected(frontend::DiagnosticsText(candidate.error().diagnostics));
      }
      auto result = compiler.Compile(candidate->snapshot().program, options.compilation);
      if (!result) {
        return std::unexpected(frontend::DiagnosticsText(result.error()));
      }
      return std::move(*result);
    }
    std::vector<frontend::SourceInput> inputs;
    inputs.reserve(sources_.size());
    for (const auto& source : sources_) {
      inputs.push_back({.path = source.path, .text = source.text});
    }
    auto result = compiler::jit::CompileModules(compiler, inputs, inputs.front().path, options);
    if (!result.Succeeded()) {
      return std::unexpected(frontend::DiagnosticsText(result.diagnostics));
    }
    return std::move(result.code);
  }

  std::expected<CompiledProgram, std::string> Compile() const {
    auto compiler = compiler::jit::Compiler::Create();
    if (!compiler) {
      return std::unexpected(frontend::DiagnosticsText(compiler.error()));
    }
    auto result = CompileImage(**compiler);
    if (!result) {
      return std::unexpected(result.error());
    }
    return CompiledProgram{
        .image = std::move(result->image),
        .resources = (*compiler)->resource_counts(),
    };
  }

  ScriptMode mode_;
  std::span<const Source> sources_;
  std::optional<std::expected<CompiledProgram, std::string>> compiled_;
};

template <class Entry>
void MeasureScript(benchmark::State& state,
                   runtime::NativeInstance& instance,
                   const ScriptRun& run,
                   const Entry& entry) {
  auto& isolate = instance.isolate();
  HandleScope scope(isolate);
  if (run.query) {
    const auto verifier = instance.FindFunction("verify");
    if (!verifier) {
      state.SkipWithError("reflection query verifier is missing");
      return;
    }
    const auto verified = runtime::ExecuteNativeFunction(instance, *verifier);
    if (!verified.Succeeded() || verified.value.AsInteger() != run.query->matches) {
      state.SkipWithError("reflection query returned incorrect methods, order or attribute copies");
      return;
    }
    state.counters["background_members"] = static_cast<double>(run.query->background);
    state.counters["matches_per_query"] =
        run.entry == "queryMissing" ? 0 : static_cast<double>(run.query->matches);
  }
  const auto values = CreateArray<std::int64_t>(scope);
  if (!values) {
    state.SkipWithError(values.error().message);
    return;
  }
  for (std::int64_t value = 0; value < run.operations; ++value) {
    if (const auto appended = values->Push(scope, value); !appended) {
      state.SkipWithError(appended.error().message);
      return;
    }
  }
  const std::array arguments{internal::HandleAccess::Read(*values)};
  const auto warm = entry.Invoke(arguments);
  if (!warm || warm->AsInteger() != run.expected) {
    state.SkipWithError("reflection benchmark warmup result is incorrect");
    return;
  }
  auto& heap = isolate.heap();
  CollectHeapBaseline(state, heap);
  heap.ResetAllocationStatistics();
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    auto result = entry.Invoke(arguments);
    if (!result || result->AsInteger() != run.expected) {
      state.SkipWithError("reflection benchmark result is incorrect");
      return;
    }
    benchmark::DoNotOptimize(*result);
  }
  const auto operations = state.iterations() * run.operations;
  state.SetItemsProcessed(operations);
  state.counters["operations"] = static_cast<double>(run.operations);
  state.counters["result"] = static_cast<double>(warm->AsInteger());
  state.counters["expected_result"] = static_cast<double>(run.expected);
  state.counters["allocation_bytes_per_op"] =
      static_cast<double>(heap.allocation_statistics().bytes) / static_cast<double>(operations);
  const auto& image = instance.image();
  state.counters["reflection_members"] = static_cast<double>(image.reflection_members.size);
  state.counters["reflection_types"] = static_cast<double>(image.reflection_types.size);
  const auto index_prepared = instance.program().reflection != nullptr;
  state.counters["reflection_index_prepared"] = index_prepared ? 1 : 0;
  if (run.registration == Registration::Disabled) {
    if (index_prepared) {
      state.SkipWithError("unregistered reflection prepared a runtime index");
    }
    if (image.reflection_members.size != 0) {
      state.SkipWithError("unregistered reflection retained a member directory");
    }
  }
  benchmark::DoNotOptimize(ReportHeap(state, heap));
}

void MeasureClosed(benchmark::State& state,
                   runtime::NativeInstance& instance,
                   const ScriptRun& run) {
  const auto function = instance.FindFunction(run.entry);
  if (!function) {
    state.SkipWithError("closed reflection benchmark entry is missing");
    return;
  }
  const auto entry = runtime::NativeVersionEntry::Bind(instance, *function);
  if (!entry) {
    state.SkipWithError("closed reflection benchmark entry cannot be bound");
    return;
  }
  MeasureScript(state, instance, run, *entry);
}

void MeasureReloadable(benchmark::State& state,
                       runtime::Isolate& isolate,
                       const CompiledProgram& compiled,
                       const ScriptRun& run) {
  auto& dispatch = isolate.native_dispatch();
  auto prepared = dispatch.Prepare(0, compiled.image);
  if (!prepared) {
    state.SkipWithError(prepared.error().message);
    return;
  }
  const auto publication = dispatch.Publish(std::move(*prepared));
  if (!publication) {
    state.SkipWithError(publication.error().message);
    return;
  }
  const auto entry = dispatch.FindEntry("main.vela", run.entry);
  if (!entry) {
    state.SkipWithError("reloadable reflection benchmark entry is missing");
    return;
  }
  const auto target = entry->Resolve();
  if (!target) {
    state.SkipWithError("reloadable reflection benchmark entry cannot be resolved");
    return;
  }
  auto instance = runtime::NativeInstance::Borrow(*target->state);
  MeasureScript(state, instance, run, *entry);
}

void ScriptWorkload(benchmark::State& state, ScriptProgram& program, const ScriptRun& run) {
  const auto prepared = program.Prepare();
  if (!prepared) {
    state.SkipWithError(prepared.error());
    return;
  }
  const auto& compiled = **prepared;
  runtime::Isolate isolate;
  if (compiled.image->IsReloadable()) {
    MeasureReloadable(state, isolate, compiled, run);
  } else {
    const auto instance = runtime::NativeInstance::Instantiate(isolate, compiled.image);
    if (!instance) {
      state.SkipWithError(instance.error().message);
      return;
    }
    MeasureClosed(state, **instance, run);
  }
  const auto& resources = compiled.resources;
  state.counters["image_bytes"] =
      static_cast<double>(resources.image_data_bytes + resources.read_only_bytes);
  state.counters["executable_bytes"] = static_cast<double>(resources.executable_bytes);
}

void AotWorkload(benchmark::State& state, const Program& program, const ScriptRun& run) {
  runtime::Isolate isolate;
  const auto instance = runtime::NativeInstance::Instantiate(isolate, *program.load_image());
  if (!instance) {
    state.SkipWithError(instance.error().message);
    return;
  }
  MeasureClosed(state, **instance, run);
  state.counters["aot_object_bytes"] = static_cast<double>(program.object_bytes);
}

constexpr std::int64_t kHostFields = 64;

ScriptProgram& HostProgram() {
  static const auto source = [] {
    std::string text = "[Reflect] class Counter {";
    for (std::int64_t index = 0; index < kHostFields; ++index) {
      text += " value" + std::to_string(index) + ": int = 0;";
    }
    return text + "}";
  }();
  static const std::array sources{Source{.path = "main.vela", .text = source}};
  static ScriptProgram program(
      {.name = "closed", .program = NativeProgramMode::Closed, .tier = NativeCodeTier::Optimized},
      sources);
  return program;
}

void HostMemberEnumeration(benchmark::State& state) {
  const auto compiled = HostProgram().Prepare();
  if (!compiled) {
    state.SkipWithError(compiled.error());
    return;
  }
  runtime::Isolate isolate;
  const auto instance = runtime::NativeInstance::Instantiate(isolate, (*compiled)->image);
  if (!instance) {
    state.SkipWithError(instance.error().message);
    return;
  }
  const auto reflection = Reflection::Bind(**instance);
  if (!reflection) {
    state.SkipWithError(reflection.error().message);
    return;
  }
  const auto type = reflection->FindType("main.vela", "Counter");
  if (!type || !*type) {
    state.SkipWithError("host reflection benchmark type is missing");
    return;
  }
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    auto fields = (*type)->GetFields();
    if (!fields || fields->size() != kHostFields) {
      state.SkipWithError("host reflection enumeration returned incorrect fields");
      return;
    }
    benchmark::DoNotOptimize(*fields);
  }
  state.SetItemsProcessed(state.iterations() * kHostFields);
}

void HostInstantiation(benchmark::State& state) {
  const auto compiled = HostProgram().Prepare();
  if (!compiled) {
    state.SkipWithError(compiled.error());
    return;
  }
  runtime::Isolate isolate;
  const auto retained = runtime::NativeInstance::Instantiate(isolate, (*compiled)->image);
  if (!retained) {
    state.SkipWithError(retained.error().message);
    return;
  }
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    auto instance = runtime::NativeInstance::Instantiate(isolate, (*compiled)->image);
    if (!instance) {
      state.SkipWithError(instance.error().message);
      return;
    }
    benchmark::DoNotOptimize(*instance);
  }
}

int RegisterScripts() {
  constexpr std::array kModes{
      ScriptMode{
          .name = "closed",
          .program = NativeProgramMode::Closed,
          .tier = NativeCodeTier::Optimized,
      },
      ScriptMode{
          .name = "baseline",
          .program = NativeProgramMode::Reloadable,
          .tier = NativeCodeTier::Baseline,
      },
      ScriptMode{
          .name = "optimized",
          .program = NativeProgramMode::Reloadable,
          .tier = NativeCodeTier::Optimized,
      },
  };
  for (const auto& program : reflection_benchmark::kPrograms) {
    for (const auto& run : program.cases) {
      benchmark::RegisterBenchmark("reflection/aot/" + std::string(run.path), AotWorkload, program,
                                   run)
          ->UseRealTime();
    }
    for (const auto mode : kModes) {
      const auto compiled = std::make_shared<ScriptProgram>(mode, program.sources);
      for (const auto& run : program.cases) {
        const auto name = "reflection/" + std::string(mode.name) + "/" + std::string(run.path);
        benchmark::RegisterBenchmark(name, [compiled, run](benchmark::State& state) {
          ScriptWorkload(state, *compiled, run);
        })->UseRealTime();
      }
    }
  }
  return 0;
}

void ReloadResources(benchmark::State& state) {
  compiler::DevelopmentOptions options;
  options.compilation.tier = NativeCodeTier::Optimized;
  auto created = compiler::DevelopmentSession::Create("main.vela", {}, std::move(options));
  if (!created) {
    state.SkipWithError(frontend::DiagnosticsText(created.error()));
    return;
  }
  auto& session = **created;
  const auto source = [](std::int64_t version) {
    return "[Reflect] class Counter { value: int = 0; add(value: int): int { return value + " +
           std::to_string(version) + "; } }";
  };
  if (const auto published =
          PublishDevelopment(session, "main.vela", source(0), compiler::DevelopmentOperation::Load);
      !published) {
    state.SkipWithError(published.error());
    return;
  }
  const auto reflection = Reflection::Bind(session.isolate().native_dispatch());
  if (!reflection) {
    state.SkipWithError(reflection.error().message);
    return;
  }
  const auto type = reflection->FindType("main.vela", "Counter");
  if (!type || !*type) {
    state.SkipWithError("cannot find registered reflection benchmark type");
    return;
  }
  const auto method = (*type)->FindMethod("add");
  const auto ctor = (*type)->GetConstructor();
  if (!method || !*method || !ctor || !*ctor) {
    state.SkipWithError("cannot find registered reflection benchmark members");
    return;
  }
  HandleScope scope(session.isolate());
  const auto receiver = (*ctor)->Invoke(scope);
  constexpr std::int64_t kArgument = 40;
  const auto value = CreateValue<std::int64_t>(scope, kArgument);
  if (!receiver || !value) {
    state.SkipWithError("cannot prepare reflection reload handles");
    return;
  }
  const std::array arguments{CallArgument(*value)};
  constexpr std::int64_t kReloads = 16;
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    for (std::int64_t index = 0; index < kReloads; ++index) {
      if (const auto published = PublishDevelopment(session, "main.vela", source(index),
                                                    compiler::DevelopmentOperation::HotReload);
          !published) {
        state.SkipWithError(published.error());
        return;
      }
      const auto result = (*method)->Invoke(scope, *receiver, arguments);
      if (!result || internal::HandleAccess::Read(*result).AsInteger() != kArgument + index) {
        state.SkipWithError("old reflection handle selected the wrong code version");
        return;
      }
      session.isolate().heap().Collect();
      if (session.code_resources().images != 1) {
        state.SkipWithError("reflection handles retained a retired code image");
        return;
      }
    }
  }
  const auto resources = session.code_resources();
  state.SetItemsProcessed(state.iterations() * kReloads);
  state.counters["operations"] = kReloads;
  state.counters["result"] = kReloads;
  state.counters["expected_result"] = kReloads;
  state.counters["retained_images"] = static_cast<double>(resources.images);
  state.counters["image_bytes"] = static_cast<double>(resources.image_data_bytes);
  state.counters["executable_bytes"] = static_cast<double>(resources.executable_bytes);
  benchmark::DoNotOptimize(ReportHeap(state, session.isolate().heap()));
}

[[maybe_unused]] const int kRegisteredScripts = RegisterScripts();
BENCHMARK(ReloadResources)->Name("reflection/reload/resources")->Iterations(1)->UseRealTime();
BENCHMARK(HostMemberEnumeration)->Name("reflection_host/enumerate")->UseRealTime();
BENCHMARK(HostInstantiation)->Name("reflection_host/instantiate")->UseRealTime();

}  // namespace
}  // namespace vela::benchmarks
