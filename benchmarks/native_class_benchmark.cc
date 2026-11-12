#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>
#include <llvm/Support/Process.h>

#include "base/execution_status.h"
#include "base/file.h"
#include "base/native_program_mode.h"
#include "base/path.h"
#include "benchmarks/native_class/entries.h"
#include "benchmarks/native_class/nativeclass_bindings.h"
#include "benchmarks/native_class/paths.h"
#include "benchmarks/native_class_host.h"
#include "compiler/development/source_session.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/jit/compiler.h"
#include "compiler/jit/source_compiler.h"
#include "compiler/native/native_roots.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_call.h"
#include "runtime/native/native_context.h"
#include "runtime/native/native_dispatch.h"
#include "runtime/native/native_function.h"
#include "runtime/native/native_image.h"
#include "runtime/native/native_instance.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"
#include "vela/error.h"

namespace vela::benchmarks::native_class {
namespace {

constexpr double kArithmeticSeriesDivisor = 2.0;

enum class Mode : std::uint8_t { Aot, Baseline, Optimized, ReloadBaseline, ReloadOptimized };
enum class Path : std::uint8_t { Class, Free };

void FreeEmpty(const NativeClassHost& object) noexcept {
  object.Empty();
}
std::int64_t FreeInteger(const NativeClassHost& object, std::int64_t value) noexcept {
  return object.Integer(value);
}
double FreeFloating(const NativeClassHost& object, double value) noexcept {
  return object.Floating(value);
}
bool FreeBoolean(const NativeClassHost& object, bool value) noexcept {
  return object.Boolean(value);
}
std::int64_t FreeNullable(const NativeClassHost& object, const NativeClassHost* other) noexcept {
  return object.Nullable(other);
}
void FreeWrite(NativeClassHost& object, std::int64_t value) noexcept {
  object.Write(value);
}
std::int64_t FreeGet(const NativeClassHost& object) noexcept {
  return object.Value();
}
void FreeSet(NativeClassHost& object, std::int64_t value) noexcept {
  object.SetValue(value);
}
std::expected<std::int64_t, vela::Error> FreeFailure(const NativeClassHost& object,
                                                     bool fail) noexcept {
  return object.Failure(fail);
}
std::int64_t FreeCollect(runtime::NativeContext& context,
                         NativeClassHost& object,
                         std::int64_t delta) noexcept {
  return object.Collect(context, delta);
}
std::int64_t FreeReenter(const NativeClassHost& object, std::int64_t value) noexcept {
  return object.Reenter(value);
}

std::expected<runtime::NativeConstructed<NativeClassHost>, vela::Error> Construct(
    runtime::NativeContext& context,
    std::int64_t initial) noexcept {
  return context.Construct<NativeClassHost>(initial);
}

bool Register(runtime::Isolate& isolate) {
  if (!bindings::module_nativeclass::Register(isolate)) {
    return false;
  }
  std::vector<runtime::NativeFunction> functions;
  functions.push_back(runtime::NativeFunction::Bind<FreeEmpty>("classbench.empty"));
  functions.push_back(runtime::NativeFunction::Bind<FreeInteger>("classbench.integer"));
  functions.push_back(runtime::NativeFunction::Bind<FreeFloating>("classbench.floating"));
  functions.push_back(runtime::NativeFunction::Bind<FreeBoolean>("classbench.boolean"));
  functions.push_back(runtime::NativeFunction::Bind<FreeNullable>("classbench.nullable"));
  functions.push_back(runtime::NativeFunction::Bind<FreeWrite>("classbench.void"));
  functions.push_back(runtime::NativeFunction::Bind<FreeGet>("classbench.get"));
  functions.push_back(runtime::NativeFunction::Bind<FreeSet>("classbench.set"));
  functions.push_back(runtime::NativeFunction::Bind<&NativeClassHost::Static>("classbench.static"));
  functions.push_back(runtime::NativeFunction::Bind<&Construct>("classbench.construct"));
  functions.push_back(runtime::NativeFunction::Bind<&NativeClassHost::Unique>("classbench.unique"));
  functions.push_back(runtime::NativeFunction::Bind<&NativeClassHost::Shared>("classbench.shared"));
  functions.push_back(
      runtime::NativeFunction::Bind<&NativeClassHost::Repeated>("classbench.repeated"));
  functions.push_back(runtime::NativeFunction::Bind<FreeFailure>("classbench.failure"));
  functions.push_back(runtime::NativeFunction::Bind<FreeCollect>("classbench.gc"));
  functions.push_back(runtime::NativeFunction::Bind<FreeReenter>("classbench.reentry"));
  for (auto& function : functions) {
    if (!isolate.RegisterNative(std::move(function))) {
      return false;
    }
  }
  return true;
}

struct Prepared final {
  std::shared_ptr<const runtime::NativeImage> image;
  std::shared_ptr<compiler::SourceSession> source;
  std::string error;
  std::vector<compiler::native::NativeFunctionMetrics> metrics;
};

compiler::jit::CompileOptions CompileOptions(Mode mode) {
  compiler::jit::CompileOptions options;
  options.tier = mode == Mode::Baseline || mode == Mode::ReloadBaseline ? NativeCodeTier::Baseline
                                                                        : NativeCodeTier::Optimized;
  if (const auto directory = llvm::sys::Process::GetEnv("VELA_NATIVE_CLASS_CODE_CACHE")) {
    options.code_cache = Utf8Path(*directory) / std::to_string(static_cast<unsigned>(mode));
    options.preserve_ir = true;
  }
  return options;
}

std::expected<void, std::string> SaveInspection(const compiler::jit::CompiledImage& compiled,
                                                const compiler::jit::CompileOptions& options) {
  if (!options.code_cache) {
    return {};
  }
  if (const auto saved = WriteFileAtomically(*options.code_cache / "image.ll",
                                             std::as_bytes(std::span(compiled.llvm_ir)));
      !saved) {
    return saved;
  }
  std::ostringstream functions;
  const auto entries = compiled.image->functions.values();
  for (std::size_t index = 0; index < entries.size(); ++index) {
    const auto& metrics = compiled.function_metrics[index];
    functions << index << '\t' << entries[index].name.text() << '\t' << metrics.shadow_slots << '\t'
              << metrics.root_stores << '\t' << metrics.frame_bytes << '\n';
  }
  const auto text = functions.str();
  return WriteFileAtomically(*options.code_cache / "functions.tsv", std::as_bytes(std::span(text)));
}

Prepared PrepareReloadable(compiler::jit::Compiler& compiler,
                           const compiler::jit::CompileOptions& options) {
  const auto loaded = frontend::LoadProgram(kWorkloadPath);
  Prepared result;
  result.source = std::make_shared<compiler::SourceSession>(std::string(kWorkloadPath));
  compiler::SourceUpdate update;
  for (const auto& source : loaded.program.sources) {
    if (!source.path.starts_with("builtin:")) {
      update.changes.push_back({.path = source.path, .text = std::string(source.text)});
    }
  }
  auto candidate = result.source->Prepare(std::move(update));
  if (!candidate) {
    result.error = frontend::DiagnosticsText(candidate.error().diagnostics);
    return result;
  }
  auto compiled = compiler.Compile(candidate->snapshot().program, options);
  if (!compiled) {
    result.error = frontend::DiagnosticsText(compiled.error());
    return result;
  }
  if (const auto saved = SaveInspection(*compiled, options); !saved) {
    result.error = saved.error();
    return result;
  }
  if (!result.source->Commit(*candidate)) {
    result.error = "benchmark source commit failed";
    return result;
  }
  result.image = std::move(compiled->image);
  result.metrics = std::move(compiled->function_metrics);
  return result;
}

const Prepared& Prepare(Mode mode) {
  static std::map<Mode, Prepared> prepared;
  if (const auto existing = prepared.find(mode); existing != prepared.end()) {
    return existing->second;
  }
  Prepared result;
  if (mode == Mode::Aot) {
    result.image = std::shared_ptr<const runtime::NativeImage>(kImage, [](const auto*) {});
  } else {
    auto compiler = compiler::jit::Compiler::Create();
    if (!compiler) {
      result.error = frontend::DiagnosticsText(compiler.error());
      return prepared.emplace(mode, std::move(result)).first->second;
    }
    const auto options = CompileOptions(mode);
    if (mode == Mode::Baseline || mode == Mode::Optimized) {
      auto compiled = compiler::jit::CompileFile(
          **compiler, kWorkloadPath, {.diagnostics = {}, .compilation = options, .reflection = {}});
      if (compiled.Succeeded()) {
        if (const auto saved = SaveInspection(compiled.code, options); !saved) {
          result.error = saved.error();
          return prepared.emplace(mode, std::move(result)).first->second;
        }
      }
      result.metrics = std::move(compiled.code.function_metrics);
      result.image = std::move(compiled.code.image);
      result.error = frontend::DiagnosticsText(compiled.diagnostics);
    } else {
      result = PrepareReloadable(**compiler, options);
    }
  }
  return prepared.emplace(mode, std::move(result)).first->second;
}

struct ReentryState final {
  runtime::NativeExecution* execution = nullptr;
  std::uint32_t function = 0;
  std::optional<runtime::NativeNamedEntry> entry;
  bool failed = false;
};

std::int64_t Reenter(void* state, std::int64_t value) noexcept {
  auto& context = *static_cast<ReentryState*>(state);
  const std::array arguments{runtime::Value::Integer(value)};
  const auto result = context.entry ? context.entry->Invoke(arguments)
                                    : runtime::InvokeNativeFunction(*context.execution,
                                                                    context.function, arguments);
  if (!result) {
    context.failed = true;
    return 0;
  }
  return result->AsInteger();
}

bool Matches(Case problem, const std::expected<runtime::Value, ExecutionStatus>& result) {
  return result &&
         (problem.floating ? result->AsDouble() == problem.expected
                           : static_cast<double>(result->AsInteger()) == problem.expected);
}

void RecordEntryMetrics(benchmark::State& state, const Prepared& prepared, std::uint32_t function) {
  const auto& metrics = prepared.metrics;
  if (metrics.empty()) {
    return;
  }
  const auto& entry = metrics[function];
  state.counters["entry_shadow_slots"] = static_cast<double>(entry.shadow_slots);
  state.counters["entry_frame_bytes"] = static_cast<double>(entry.frame_bytes);
  state.counters["entry_root_stores"] = static_cast<double>(entry.root_stores);
}

void ReportHeapRetention(benchmark::State& state, runtime::Heap& heap, std::size_t baseline) {
  // 先结束计时期间的增量轮次，再以当前根启动完整回收，排除旧标记留下的浮动垃圾。
  heap.FinishCollection();
  heap.Collect();
  const auto retained = heap.allocated_bytes();
  state.counters["retained_bytes_delta"] =
      static_cast<double>(retained) - static_cast<double>(baseline);
  if (retained != baseline) {
    state.SkipWithError("native class benchmark retained heap storage after full GC");
  }
}

void Script(benchmark::State& state, Case problem, Mode mode, Path path) {
  const auto& prepared = Prepare(mode);
  if (!prepared.image) {
    state.SkipWithError(prepared.error);
    return;
  }
  runtime::Isolate isolate;
  if (!Register(isolate)) {
    state.SkipWithError("native class benchmark registration failed");
    return;
  }
  static_cast<void>(isolate.FreezeBindings());
  const auto instantiation_started = std::chrono::steady_clock::now();
  const auto name =
      std::string(path == Path::Class ? "class_" : "free_") + std::string(problem.name);
  std::unique_ptr<runtime::NativeInstance> instance;
  std::unique_ptr<runtime::NativeExecution> execution;
  std::optional<runtime::NativeNamedEntry> entry;
  ReentryState reentry;
  std::uint32_t function = 0;
  std::expected<runtime::Value, ExecutionStatus> receiver;
  if (prepared.source) {
    auto& dispatch = isolate.native_dispatch();
    auto publication = dispatch.Prepare(0, prepared.image);
    if (!publication || !dispatch.Publish(std::move(*publication))) {
      state.SkipWithError("native class benchmark publication failed");
      return;
    }
    auto selected = dispatch.FindEntry(kWorkloadPath, name);
    auto nested = dispatch.FindEntry(kWorkloadPath, "nested");
    const auto get_receiver = dispatch.FindEntry(kWorkloadPath, "receiver");
    if (!selected || !get_receiver || !nested) {
      state.SkipWithError("missing reloadable class benchmark entry");
      return;
    }
    entry = std::move(*selected);
    reentry.entry = std::move(*nested);
    receiver = get_receiver->Invoke({});
  } else {
    auto created = runtime::NativeInstance::Instantiate(isolate, prepared.image);
    if (!created) {
      state.SkipWithError(created.error().message);
      return;
    }
    instance = std::move(*created);
    execution = std::make_unique<runtime::NativeExecution>(*instance);
    const auto selected = instance->FindFunction(name);
    const auto get_receiver = instance->FindFunction("receiver");
    const auto nested = instance->FindFunction("nested");
    if (!selected || !get_receiver || !nested) {
      state.SkipWithError("missing closed class benchmark entry");
      return;
    }
    function = *selected;
    RecordEntryMetrics(state, prepared, function);
    reentry.execution = execution.get();
    reentry.function = *nested;
    receiver = runtime::InvokeNativeTarget(*execution, *get_receiver, {});
  }
  if (!receiver) {
    state.SkipWithError("native class benchmark receiver initialization failed");
    return;
  }
  auto* object = static_cast<NativeClassHost*>(runtime::HostObjectPointer(*receiver->AsObject()));
  object->SetReentry(&Reenter, &reentry);
  const auto invoke = [&] {
    return entry ? entry->Invoke(std::span<const runtime::Value>{})
                 : runtime::InvokeNativeTarget(*execution, function, {});
  };
  state.counters["instantiation_ns"] =
      static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now() - instantiation_started)
                              .count());
  const auto first_started = std::chrono::steady_clock::now();
  if (!Matches(problem, invoke())) {
    state.SkipWithError("native class benchmark warm-up checksum mismatch");
    return;
  }
  state.counters["first_execution_ns"] =
      static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now() - first_started)
                              .count());
  auto repeated = NativeClassHost::Repeated();
  const auto owners = repeated.use_count();
  auto& heap = isolate.heap();
  heap.FinishCollection();
  heap.Collect();
  const auto baseline = heap.allocated_bytes();
  const auto counts = NativeClassHost::AllocationCounts();
  heap.ResetAllocationStatistics();
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    auto result = invoke();
    benchmark::DoNotOptimize(result);
    if (!Matches(problem, result) || reentry.failed) {
      state.SkipWithError("native class benchmark checksum or reentry failed");
      return;
    }
  }
  const auto allocations = heap.allocation_statistics();
  const auto finished = NativeClassHost::AllocationCounts();
  const auto iterations = static_cast<double>(state.iterations());
  state.counters["calls_per_iteration"] = static_cast<double>(problem.count);
  state.counters["allocations_per_iteration"] = static_cast<double>(allocations.count) / iterations;
  state.counters["constructed_per_iteration"] =
      static_cast<double>(finished.constructed - counts.constructed) / iterations;
  state.counters["peak_live_bytes"] = static_cast<double>(allocations.peak_live_bytes);
  state.counters["shared_owner_delta"] = static_cast<double>(repeated.use_count() - owners);
  ReportHeapRetention(state, heap, baseline);
  const auto destroyed = NativeClassHost::AllocationCounts().destroyed - counts.destroyed;
  state.counters["destroyed_per_iteration"] = static_cast<double>(destroyed) / iterations;
  if (destroyed != finished.constructed - counts.constructed) {
    state.SkipWithError("native class benchmark retained a constructed object after full GC");
  }
  state.SetItemsProcessed(state.iterations() * problem.count);
}

void Cpp(benchmark::State& state, bool adapter) {
  NativeClassHost object(1);
  auto* pointer = &NativeClassIntegerPointer;
  auto* handwritten = &NativeClassIntegerAdapter;
  benchmark::DoNotOptimize(pointer);
  benchmark::DoNotOptimize(handwritten);
  static constexpr std::int64_t kCount = 4096;
  constexpr auto kExpected = kCount * (kCount + 1) / 2;
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    std::int64_t sum = 0;
    for (std::int64_t index = 0; index < kCount; ++index) {
      sum += adapter ? handwritten(object, index) : pointer(&object, index);
    }
    benchmark::DoNotOptimize(sum);
    if (sum != kExpected) {
      state.SkipWithError("native class C++ baseline checksum mismatch");
      return;
    }
  }
  state.counters["calls_per_iteration"] = static_cast<double>(kCount);
  state.SetItemsProcessed(state.iterations() * kCount);
}

enum class CppKernel : std::uint8_t {
  Empty,
  Floating,
  Boolean,
  Void,
  Getter,
  Setter,
  Static,
  Nullable,
  Alternating,
};

template <CppKernel Kernel>
using CppResult = std::conditional_t<Kernel == CppKernel::Floating, double, std::int64_t>;

template <CppKernel Kernel>
CppResult<Kernel> CppPointer(NativeClassHost* object, std::int64_t value) noexcept {
  if constexpr (Kernel == CppKernel::Empty) {
    object->Empty();
    return value + 1;
  } else if constexpr (Kernel == CppKernel::Floating) {
    return object->Floating(static_cast<double>(value));
  } else if constexpr (Kernel == CppKernel::Boolean) {
    return object->Boolean(value % 2 == 0) ? 1 : 0;
  } else if constexpr (Kernel == CppKernel::Void) {
    object->Write(value);
    return value;
  } else if constexpr (Kernel == CppKernel::Getter) {
    return object->Value();
  } else if constexpr (Kernel == CppKernel::Setter) {
    object->SetValue(value);
    return value;
  } else if constexpr (Kernel == CppKernel::Static) {
    return NativeClassHost::Static(value);
  } else if constexpr (Kernel == CppKernel::Nullable) {
    return object->Nullable(value % 2 == 0 ? object : nullptr);
  } else {
    return object->Integer(value);
  }
}

template <CppKernel Kernel>
CppResult<Kernel> CppAdapter(NativeClassHost& object, std::int64_t value) noexcept {
  return CppPointer<Kernel>(&object, value);
}

template <CppKernel Kernel>
void CppScalar(benchmark::State& state, bool adapter) {
  NativeClassHost first(1);
  NativeClassHost second(2);
  auto* pointer = &CppPointer<Kernel>;
  auto* handwritten = &CppAdapter<Kernel>;
  benchmark::DoNotOptimize(pointer);
  benchmark::DoNotOptimize(handwritten);
  static constexpr std::int64_t kCount = 4096;
  const auto expected = [] -> CppResult<Kernel> {
    if constexpr (Kernel == CppKernel::Floating) {
      return static_cast<double>(kCount) * static_cast<double>(kCount + 2) /
             kArithmeticSeriesDivisor;
    } else if constexpr (Kernel == CppKernel::Alternating) {
      return kCount * (kCount + 2) / 2;
    } else if constexpr (Kernel == CppKernel::Boolean) {
      return kCount / 2;
    } else if constexpr (Kernel == CppKernel::Getter || Kernel == CppKernel::Nullable) {
      return kCount;
    } else if constexpr (Kernel == CppKernel::Setter || Kernel == CppKernel::Void) {
      return kCount * (kCount - 1) / 2;
    } else {
      return kCount * (kCount + 1) / 2;
    }
  }();
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    CppResult<Kernel> sum = 0;
    for (std::int64_t index = 0; index < kCount; ++index) {
      auto* object = &first;
      if constexpr (Kernel == CppKernel::Alternating) {
        object = index % 2 == 0 ? &first : &second;
      }
      sum += adapter ? handwritten(*object, index) : pointer(object, index);
    }
    benchmark::DoNotOptimize(sum);
    if (sum != expected) {
      state.SkipWithError("native class C++ scalar baseline checksum mismatch");
      return;
    }
  }
  state.counters["calls_per_iteration"] = static_cast<double>(kCount);
  state.SetItemsProcessed(state.iterations() * kCount);
}

template <CppKernel Kernel>
void RegisterCpp(std::string_view name) {
  benchmark::RegisterBenchmark("native_class/cpp_pointer/" + std::string(name), CppScalar<Kernel>,
                               false)
      ->UseRealTime();
  benchmark::RegisterBenchmark("native_class/cpp_adapter/" + std::string(name), CppScalar<Kernel>,
                               true)
      ->UseRealTime();
}

int RegisterBenchmarks() {
  const std::array modes{
      std::pair{Mode::Aot, "aot"},
      std::pair{Mode::Baseline, "baseline"},
      std::pair{Mode::Optimized, "optimized"},
      std::pair{Mode::ReloadBaseline, "reload_baseline"},
      std::pair{Mode::ReloadOptimized, "reload_optimized"},
  };
  for (const auto& [mode, name] : modes) {
    for (const auto& problem : kCases) {
      const auto prefix = "native_class/" + std::string(name);
      benchmark::RegisterBenchmark(prefix + "/class/" + std::string(problem.name), Script, problem,
                                   mode, Path::Class)
          ->UseRealTime();
      benchmark::RegisterBenchmark(prefix + "/free/" + std::string(problem.name), Script, problem,
                                   mode, Path::Free)
          ->UseRealTime();
    }
  }
  benchmark::RegisterBenchmark("native_class/cpp_pointer/integer", Cpp, false)->UseRealTime();
  benchmark::RegisterBenchmark("native_class/cpp_adapter/integer", Cpp, true)->UseRealTime();
  RegisterCpp<CppKernel::Empty>("empty");
  RegisterCpp<CppKernel::Floating>("floating");
  RegisterCpp<CppKernel::Boolean>("boolean");
  RegisterCpp<CppKernel::Void>("void");
  RegisterCpp<CppKernel::Getter>("getter");
  RegisterCpp<CppKernel::Setter>("setter");
  RegisterCpp<CppKernel::Static>("static");
  RegisterCpp<CppKernel::Nullable>("nullable");
  RegisterCpp<CppKernel::Alternating>("alternating");
  return 0;
}

[[maybe_unused]] const int kRegistered = RegisterBenchmarks();

}  // namespace
}  // namespace vela::benchmarks::native_class
