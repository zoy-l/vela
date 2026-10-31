#include "benchmarks/map_literal_benchmark.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <benchmark/benchmark.h>

#include "base/execution_status.h"
#include "base/native_program_mode.h"
#include "benchmarks/map_literal_native/entries.h"
#include "compiler/aot/native_compiler.h"
#include "compiler/compiler.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/source_loader.h"
#include "compiler/ir/preparation.h"
#include "compiler/jit/compiler.h"
#include "compiler/source_session.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/native_abi.h"
#include "runtime/native_call.h"
#include "runtime/native_dispatch.h"
#include "runtime/native_instance.h"
#include "runtime/value.h"
#include "vm/bytecode.h"
#include "vm/vm.h"

namespace vela::benchmarks {
namespace {

constexpr std::string_view kMapPath = "map.vela";
using Clock = std::chrono::steady_clock;
enum class MapBackend : std::uint8_t { Vm, Aot, Baseline, Optimized };
constexpr auto kCompileBackends = std::to_array<std::pair<MapBackend, std::string_view>>({
    {MapBackend::Vm, "vm"},
    {MapBackend::Aot, "aot"},
    {MapBackend::Baseline, "baseline"},
    {MapBackend::Optimized, "optimized"},
});

template <class Invoke>
void RunMap(benchmark::State& state,
            const MapLiteralCase& problem,
            runtime::Heap& heap,
            Invoke invoke) {
  const auto check = [&]() -> std::expected<void, std::string> {
    auto result = invoke();
    if (!result) {
      return std::unexpected(result.error());
    }
    if (!result->IsInteger() || result->AsInteger() != problem.expected) {
      return std::unexpected("map checksum mismatch");
    }
    benchmark::DoNotOptimize(*result);
    return {};
  };
  if (const auto warm = check(); !warm) {
    state.SkipWithError(warm.error());
    return;
  }
  // 独立执行样本禁用自动 GC；测量完整构造与遍历所申请的活跃对象及存储。
  heap.Collect();
  const auto baseline_bytes = heap.allocated_bytes();
  const auto baseline_objects = heap.object_count();
  if (const auto sample = check(); !sample) {
    state.SkipWithError(sample.error());
    return;
  }
  state.counters["allocation_bytes"] = static_cast<double>(heap.allocated_bytes() - baseline_bytes);
  state.counters["allocated_objects"] = static_cast<double>(heap.object_count() - baseline_objects);
  const auto before = heap.object_count();
  const auto collecting = Clock::now();
  heap.Collect();
  state.counters["sample_collection_ns"] = static_cast<double>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - collecting).count());
  state.counters["sample_collected_objects"] = static_cast<double>(before - heap.object_count());
  state.counters["retained_bytes"] = static_cast<double>(heap.allocated_bytes() - baseline_bytes);
  state.counters["result"] = static_cast<double>(problem.expected);
  state.counters["source_bytes"] = static_cast<double>(problem.source.size());
  for (auto iteration : state) {
    state.PauseTiming();
    heap.Collect();
    state.ResumeTiming();
    if (const auto result = check(); !result) {
      state.SkipWithError(result.error());
      break;
    }
  }
}

void MapVm(benchmark::State& state, MapLiteralCase problem) {
  runtime::Isolate isolate(std::numeric_limits<std::size_t>::max());
  const auto compiled = compiler::CompileSource(problem.source, isolate);
  if (!compiled.Succeeded()) {
    state.SkipWithError(frontend::DiagnosticsText(compiled.diagnostics));
    return;
  }
  const auto entry = compiled.program.FindFunction("main");
  vm::VirtualMachine machine(isolate);
  RunMap(state, problem, isolate.heap(), [&]() -> std::expected<runtime::Value, std::string> {
    const auto result = machine.Execute(entry);
    return result.Succeeded() ? std::expected<runtime::Value, std::string>(result.value)
                              : std::unexpected(result.message);
  });
  std::size_t bytes = 0;
  for (const auto& function : compiled.program.functions()) {
    bytes += function.code.size() * sizeof(vm::Instruction);
  }
  state.counters["code_bytes"] = static_cast<double>(bytes);
  state.counters["register_bytes"] = static_cast<double>(machine.register_bytes());
}

void MapAot(benchmark::State& state, MapLiteralCase problem) {
  runtime::Isolate isolate(std::numeric_limits<std::size_t>::max());
  const auto instance = runtime::NativeInstance::Instantiate(isolate, *kMapLiteralImage);
  if (!instance) {
    state.SkipWithError(instance.error().message);
    return;
  }
  runtime::NativeExecution execution(**instance);
  RunMap(state, problem, isolate.heap(), [&]() -> std::expected<runtime::Value, std::string> {
    const auto result = runtime::InvokeNativeTarget(execution, problem.function, {});
    return result ? std::expected<runtime::Value, std::string>(*result)
                  : std::unexpected(std::string(ExecutionStatusName(result.error())));
  });
  state.counters["frame_bytes"] = static_cast<double>(problem.frame_bytes);
  state.counters["shadow_slots"] = static_cast<double>(problem.shadow_slots);
  state.counters["root_stores"] = static_cast<double>(problem.root_stores);
}

void MapJit(benchmark::State& state, MapLiteralCase problem, NativeCodeTier tier) {
  compiler::SourceSession source{std::string(kMapPath)};
  const auto prepared = source.Prepare({
      .base_revision = 0,
      .changes = {{.path = std::string(kMapPath), .text = std::string(problem.source)}},
      .added_roots = {},
  });
  if (!prepared) {
    state.SkipWithError(frontend::DiagnosticsText(prepared.error().diagnostics));
    return;
  }
  auto compiler = compiler::jit::Compiler::Create();
  if (!compiler) {
    state.SkipWithError(frontend::DiagnosticsText(compiler.error()));
    return;
  }
  const auto compiled = (*compiler)->Compile(prepared->snapshot().program, {.tier = tier});
  if (!compiled) {
    state.SkipWithError(frontend::DiagnosticsText(compiled.error()));
    return;
  }
  runtime::Isolate isolate(std::numeric_limits<std::size_t>::max());
  auto& dispatch = isolate.native_dispatch();
  auto publication = dispatch.Prepare(0, compiled->image);
  if (!publication) {
    state.SkipWithError(publication.error().message);
    return;
  }
  if (const auto published = dispatch.Publish(std::move(*publication)); !published) {
    state.SkipWithError(published.error().message);
    return;
  }
  const auto entry = dispatch.FindEntry(kMapPath, "main");
  if (!entry) {
    state.SkipWithError("map entry is missing");
    return;
  }
  RunMap(state, problem, isolate.heap(), [&]() -> std::expected<runtime::Value, std::string> {
    const auto result = entry->Invoke({});
    return result ? std::expected<runtime::Value, std::string>(*result)
                  : std::unexpected(std::string(ExecutionStatusName(result.error())));
  });
  state.counters["code_bytes"] =
      static_cast<double>((*compiler)->resource_counts().executable_bytes);
  for (const auto& metrics : compiled->function_metrics) {
    state.counters["frame_bytes"] += static_cast<double>(metrics.frame_bytes);
    state.counters["shadow_slots"] += static_cast<double>(metrics.shadow_slots);
    state.counters["root_stores"] += static_cast<double>(metrics.root_stores);
  }
}

std::expected<std::size_t, std::string> CompileMap(const MapLiteralCase& problem,
                                                   MapBackend backend,
                                                   compiler::jit::Compiler& compiler) {
  if (backend == MapBackend::Vm) {
    runtime::Isolate isolate;
    const auto compiled = compiler::CompileSource(problem.source, isolate);
    if (!compiled.Succeeded()) {
      return std::unexpected(frontend::DiagnosticsText(compiled.diagnostics));
    }
    std::size_t bytes = 0;
    for (const auto& function : compiled.program.functions()) {
      bytes += function.code.size() * sizeof(vm::Instruction);
    }
    return bytes;
  }
  if (backend == MapBackend::Aot) {
    const frontend::SourceInput input{.path = kMapPath, .text = problem.source};
    const auto prepared =
        ir::PrepareParsed(frontend::LoadSources(std::span(&input, 1), input.path));
    if (!prepared.Succeeded()) {
      return std::unexpected(frontend::DiagnosticsText(prepared.diagnostics));
    }
    const auto compiled = compiler::aot::Compile(prepared.program, {.exports = {"main"}});
    if (!compiled) {
      return std::unexpected(frontend::DiagnosticsText(compiled.error()));
    }
    return compiled->object.size();
  }
  compiler::SourceSession source{std::string(kMapPath)};
  const auto prepared = source.Prepare({
      .base_revision = 0,
      .changes = {{.path = std::string(kMapPath), .text = std::string(problem.source)}},
      .added_roots = {},
  });
  if (!prepared) {
    return std::unexpected(frontend::DiagnosticsText(prepared.error().diagnostics));
  }
  const auto tier =
      backend == MapBackend::Baseline ? NativeCodeTier::Baseline : NativeCodeTier::Optimized;
  const auto compiled = compiler.Compile(prepared->snapshot().program, {.tier = tier});
  if (!compiled) {
    return std::unexpected(frontend::DiagnosticsText(compiled.error()));
  }
  return compiler.resource_counts().executable_bytes;
}

void MapCompilation(benchmark::State& state, MapLiteralCase problem, MapBackend backend) {
  auto compiler = compiler::jit::Compiler::Create();
  if (!compiler) {
    state.SkipWithError(frontend::DiagnosticsText(compiler.error()));
    return;
  }
  for (auto iteration : state) {
    auto result = CompileMap(problem, backend, **compiler);
    if (!result) {
      state.SkipWithError(result.error());
      break;
    }
    state.counters["code_bytes"] = static_cast<double>(*result);
    benchmark::DoNotOptimize(result);
  }
  state.counters["source_bytes"] = static_cast<double>(problem.source.size());
}

const auto kMapRegistration = [] {
  for (const auto& problem : kMapLiteralCases) {
    const auto suffix = std::string(problem.name);
    benchmark::RegisterBenchmark("maps/vm/" + suffix, MapVm, problem)->UseRealTime();
    benchmark::RegisterBenchmark("maps/aot/" + suffix, MapAot, problem)->UseRealTime();
    benchmark::RegisterBenchmark("maps/baseline/" + suffix, MapJit, problem,
                                 NativeCodeTier::Baseline)
        ->UseRealTime();
    benchmark::RegisterBenchmark("maps/optimized/" + suffix, MapJit, problem,
                                 NativeCodeTier::Optimized)
        ->UseRealTime();
    for (const auto& [backend, name] : kCompileBackends) {
      benchmark::RegisterBenchmark("maps/compile/" + std::string(name) + "/" + suffix,
                                   MapCompilation, problem, backend)
          ->UseRealTime();
    }
  }
  return 0;
}();

}  // namespace
}  // namespace vela::benchmarks
