#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>
#include <llvm/ExecutionEngine/JITSymbol.h>
#include <llvm/ExecutionEngine/Orc/AbsoluteSymbols.h>
#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/CoreContainers.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/Shared/ExecutorAddress.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/TargetParser/Triple.h>

#include "compiler/aot/native_compiler.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/ir/preparation.h"
#include "compiler/jit/code_resources.h"
#include "compiler/jit/engine.h"
#include "compiler/native/code_generator.h"
#include "compiler/native/escape_analysis.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_api.h"
#include "runtime/native/native_call.h"
#include "runtime/native/native_image.h"
#include "runtime/native/native_instance.h"

namespace vela::benchmarks {
namespace {

using Clock = std::chrono::steady_clock;
using compiler::native::EscapeOptimization;
enum class Workload : std::uint8_t {
  Numeric,
  References,
  RareEscape,
  AllEscape,
  Graph,
  Calls,
  Recursion,
};

struct Problem final {
  std::string_view name;
  Workload workload;
  std::int64_t count;
};

constexpr std::array kProblems{
    Problem{.name = "numeric", .workload = Workload::Numeric, .count = 256},
    Problem{.name = "references", .workload = Workload::References, .count = 256},
    Problem{.name = "rare_escape", .workload = Workload::RareEscape, .count = 256},
    Problem{.name = "all_escape", .workload = Workload::AllEscape, .count = 256},
    Problem{.name = "graph", .workload = Workload::Graph, .count = 256},
    Problem{.name = "calls", .workload = Workload::Calls, .count = 256},
    Problem{.name = "recursion", .workload = Workload::Recursion, .count = 64},
};

std::string Source(Workload workload) {
  std::string source = R"(
class Point { x: int; y: int; text: string?; static saved: Point? = null; }
class Link { value: int; next: Link?; }
func adjust(point: Point, amount: int): void { point.x += amount; }
func recurse(depth: int): int {
  const point = Point(x: depth, y: 2, text: null);
  adjust(point, 2);
  return depth == 0 ? point.x : recurse(depth - 1) + point.x;
}
export func run(count: int): int {
)";
  if (workload == Workload::Recursion) {
    return source + "return recurse(count); }";
  }
  source += "let sum = 0; for (let index = 0; index < count; index++) { ";
  if (workload == Workload::Graph) {
    return source +
           "const first = Link(value: index, next: null); "
           "const second = Link(value: 2, next: first); first.next = second; "
           "sum += first.value + (first.next as Link).value; } return sum; }";
  }
  source += "const point = Point(x: index, y: 2, text: ";
  source += workload == Workload::References ? "'a' + 'bc'); " : "null); ";
  if (workload == Workload::RareEscape) {
    source += "if ((index & 63) == 0) { Point.saved = point; } ";
  } else if (workload == Workload::AllEscape) {
    source += "Point.saved = point; ";
  } else if (workload == Workload::Calls) {
    source += "adjust(point, 2); ";
  }
  source += workload == Workload::References ? "sum += (point.text ?? '').length; "
                                             : "sum += point.x + point.y; ";
  return source + "} return sum; }";
}

std::int64_t Expected(const Problem& problem) {
  const auto count = problem.count;
  switch (problem.workload) {
    case Workload::References:
      return count * 3;
    case Workload::Recursion:
      return (count * (count + 1) / 2) + (2 * (count + 1));
    default:
      return (count * (count - 1) / 2) + (count * (problem.workload == Workload::Calls ? 4 : 2));
  }
}

struct Compiled final {
  std::unique_ptr<llvm::orc::LLJIT> engine;
  compiler::aot::NativeArtifact artifact;
  const runtime::NativeImage* image = nullptr;
  std::shared_ptr<compiler::jit::CodeResourceCounters> resources;
  std::chrono::nanoseconds compilation{};
  std::chrono::nanoseconds linking{};
};

std::expected<Compiled, std::string> Compile(const Problem& problem, EscapeOptimization mode) {
  const auto text = Source(problem.workload);
  const frontend::SourceInput input{.path = "escape_benchmark.vela", .text = text};
  auto prepared = ir::PrepareParsed(frontend::LoadSources(std::span(&input, 1), input.path));
  if (!prepared.Succeeded()) {
    return std::unexpected(prepared.diagnostics.front().message);
  }
  const auto started = Clock::now();
  auto artifact = compiler::aot::Compile(
      std::move(prepared.program),
      {.exports = {"run"}, .escape = mode, .placement = compiler::NativeCodePlacement::Dynamic});
  const auto compilation = Clock::now() - started;
  if (!artifact) {
    return std::unexpected(artifact.error().front().message);
  }
  auto engine = compiler::jit::CreateEngine(llvm::Triple(artifact->target));
  if (!engine) {
    return std::unexpected(llvm::toString(engine.takeError()));
  }
  llvm::orc::SymbolMap api;
  const auto bind = [&](const char* name, auto* function) {
    api.try_emplace((*engine)->mangleAndIntern(name), llvm::orc::ExecutorAddr::fromPtr(function),
                    llvm::JITSymbolFlags::Exported);
  };
  bind("vela_aot_v1_instantiate", &vela_aot_v1_instantiate);
  bind("vela_aot_v1_invoke", &vela_aot_v1_invoke);
  if (auto error =
          (*engine)->getMainJITDylib().define(llvm::orc::absoluteSymbols(std::move(api)))) {
    return std::unexpected(llvm::toString(std::move(error)));
  }
  auto resources = std::make_shared<compiler::jit::CodeResourceCounters>();
  compiler::jit::InstallResourceAccounting(**engine, resources);
  const auto linking = Clock::now();
  const auto bytes = std::as_bytes(std::span(artifact->object));
  const void* data = bytes.data();
  auto buffer =
      llvm::MemoryBuffer::getMemBufferCopy({static_cast<const char*>(data), bytes.size()});
  if (auto error = (*engine)->addObjectFile(std::move(buffer))) {
    return std::unexpected(llvm::toString(std::move(error)));
  }
  auto entry = (*engine)->lookup(artifact->image_symbol);
  if (!entry) {
    return std::unexpected(llvm::toString(entry.takeError()));
  }
  using ImageEntry = const runtime::NativeImage* (*)();
  const auto* image = entry->toPtr<ImageEntry>()();
  return Compiled{
      .engine = std::move(*engine),
      .artifact = std::move(*artifact),
      .image = image,
      .resources = std::move(resources),
      .compilation = compilation,
      .linking = Clock::now() - linking,
  };
}

std::expected<void, std::string> Invoke(runtime::NativeExecution& execution,
                                        const Compiled& compiled,
                                        const Problem& problem) {
  const std::array arguments{
      runtime::NativeValue{
          .payload = std::bit_cast<std::uint64_t>(problem.count),
          .kind = runtime::NativeValueKind::Integer,
      },
  };
  const auto value =
      runtime::InvokeNativeTarget(execution, compiled.artifact.exports.front().function, arguments);
  if (!value || !value->IsInteger() || value->AsInteger() != Expected(problem)) {
    return std::unexpected("escape benchmark execution or checksum failed");
  }
  benchmark::DoNotOptimize(value->AsInteger());
  return {};
}

std::expected<void, std::string> AllocationSample(benchmark::State& state,
                                                  const Compiled& compiled,
                                                  const Problem& problem) {
  // 独立实例禁用自动 GC；单次执行前后增量才是本次真实申请量。
  runtime::Isolate isolate(std::numeric_limits<std::size_t>::max());
  auto instance = runtime::NativeInstance::Instantiate(isolate, *compiled.image);
  if (!instance) {
    return std::unexpected(instance.error().message);
  }
  runtime::NativeExecution execution(**instance);
  if (const auto warm = Invoke(execution, compiled, problem); !warm) {
    return warm;
  }
  auto& heap = isolate.heap();
  heap.Collect();
  const auto bytes = heap.allocated_bytes();
  const auto fields = heap.instance_field_bytes();
  const auto objects = heap.object_count();
  if (const auto sample = Invoke(execution, compiled, problem); !sample) {
    return sample;
  }
  state.counters["allocation_bytes"] = static_cast<double>(heap.allocated_bytes() - bytes);
  state.counters["field_allocation_bytes"] =
      static_cast<double>(heap.instance_field_bytes() - fields);
  state.counters["allocated_objects"] = static_cast<double>(heap.object_count() - objects);
  const auto before_collection = heap.object_count();
  const auto started = Clock::now();
  heap.Collect();
  state.counters["sample_collection_ns"] = static_cast<double>((Clock::now() - started).count());
  state.counters["sample_collected_objects"] =
      static_cast<double>(before_collection - heap.object_count());
  return {};
}

void Escape(benchmark::State& state, Problem problem, EscapeOptimization mode) {
  auto compiled = Compile(problem, mode);
  if (!compiled) {
    state.SkipWithError(compiled.error());
    return;
  }
  if (const auto sample = AllocationSample(state, *compiled, problem); !sample) {
    state.SkipWithError(sample.error());
    return;
  }
  runtime::Isolate isolate;
  auto instance = runtime::NativeInstance::Instantiate(isolate, *compiled->image);
  if (!instance) {
    state.SkipWithError(instance.error().message);
    return;
  }
  runtime::NativeExecution execution(**instance);
  const auto& artifact = compiled->artifact;
  const auto& metrics = artifact.function_metrics.at(artifact.exports.front().function);
  const auto expected = static_cast<double>(Expected(problem));
  state.counters["result"] = expected;
  state.counters["expected_result"] = expected;
  state.counters["entry_frame_bytes"] = static_cast<double>(metrics.frame_bytes);
  state.counters["entry_shadow_slots"] = static_cast<double>(metrics.shadow_slots);
  state.counters["compile_ns"] = static_cast<double>(compiled->compilation.count());
  state.counters["preparation_ns"] = static_cast<double>(artifact.timings.preparation.count());
  state.counters["analysis_lowering_ns"] = static_cast<double>(artifact.timings.generation.count());
  state.counters["optimization_ns"] = static_cast<double>(artifact.timings.optimization.count());
  state.counters["emission_ns"] = static_cast<double>(artifact.timings.emission.count());
  state.counters["link_ns"] = static_cast<double>(compiled->linking.count());
  state.counters["object_file_bytes"] = static_cast<double>(artifact.object.size());
  state.counters["executable_bytes"] =
      static_cast<double>(compiled->resources->executable_bytes.load());
  state.counters["read_only_bytes"] =
      static_cast<double>(compiled->resources->read_only_bytes.load());
  state.counters["writable_bytes"] =
      static_cast<double>(compiled->resources->writable_bytes.load());
  for (const auto& function : artifact.function_metrics) {
    auto& maximum_frame = state.counters["maximum_frame_bytes"];
    maximum_frame.value = std::max(
        maximum_frame.value,
        static_cast<double>(std::max(function.frame_bytes, function.borrowed_frame_bytes)));
    auto& maximum_roots = state.counters["maximum_shadow_slots"];
    maximum_roots.value = std::max(
        maximum_roots.value,
        static_cast<double>(std::max(function.shadow_slots, function.borrowed_shadow_slots)));
    auto& borrowed_frame = state.counters["borrowed_frame_bytes"];
    borrowed_frame.value =
        std::max(borrowed_frame.value, static_cast<double>(function.borrowed_frame_bytes));
    auto& borrowed_roots = state.counters["borrowed_shadow_slots"];
    borrowed_roots.value =
        std::max(borrowed_roots.value, static_cast<double>(function.borrowed_shadow_slots));
    state.counters["borrowed_root_stores"] += static_cast<double>(function.borrowed_root_stores);
    state.counters["eliminated_sites"] += static_cast<double>(function.eliminated_allocations);
    state.counters["sunk_sites"] += static_cast<double>(function.sunk_allocations);
    state.counters["retained_sites"] += static_cast<double>(function.retained_allocations);
    state.counters["stack_records"] += static_cast<double>(function.stack_records);
  }
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    if (const auto value = Invoke(execution, *compiled, problem); !value) {
      state.SkipWithError(value.error());
      return;
    }
  }
}

int Register() {
  benchmark::AddCustomContext("escape_allocation_sample",
                              "separate warmed Isolate; automatic GC disabled; one invocation");
  for (const auto& problem : kProblems) {
    for (const auto& [name, mode] : std::array{
             std::pair{"heap", EscapeOptimization::Disabled},
             std::pair{"scalar", EscapeOptimization::ScalarReplacement},
             std::pair{"borrow", EscapeOptimization::Enabled},
         }) {
      benchmark::RegisterBenchmark("escape/" + std::string(name) + '/' + std::string(problem.name),
                                   Escape, problem, mode)
          ->UseRealTime();
    }
  }
  return 0;
}

[[maybe_unused]] const int kRegistered = Register();

}  // namespace
}  // namespace vela::benchmarks
