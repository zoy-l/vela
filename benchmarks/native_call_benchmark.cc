#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <type_traits>

#include "benchmark/benchmark.h"

#include "benchmarks/native_call/entries.h"
#include "benchmarks/native_call_kernels.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/jit/compiler.h"
#include "compiler/jit/source_compiler.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_execution.h"
#include "runtime/native/native_function.h"
#include "runtime/native/native_instance.h"

namespace vela::benchmarks {
namespace {

using native_call::kBorrowedResult;
using native_call::kBorrowedText;
using native_call::Workload;

enum class CallPath : std::uint8_t { Direct, Indirect };
enum class ScriptPath : std::uint8_t { Native, Control };
enum class Execution : std::uint8_t { Jit, Aot };

constexpr double kArithmeticSeriesDivisor = 2.0;

template <Workload Case>
constexpr auto Kernel() {
  if constexpr (Case == Workload::Empty) {
    return NativeEmpty;
  } else if constexpr (Case == Workload::Integer) {
    return NativeInteger;
  } else if constexpr (Case == Workload::Double) {
    return NativeDouble;
  } else {
    return NativeStringBorrow;
  }
}

template <Workload Case>
using Sum = std::conditional_t<Case == Workload::Double, double, std::int64_t>;

template <Workload Case>
Sum<Case> Expected(std::int64_t count) {
  if constexpr (Case == Workload::Double) {
    return (static_cast<double>(count * (count - 1)) / kArithmeticSeriesDivisor) +
           (static_cast<double>(count) * kNativeDoubleIncrement);
  } else if constexpr (Case == Workload::StringBorrow) {
    return count * kBorrowedResult;
  } else {
    return count * (count + 1) / 2;
  }
}

void ReportCalls(benchmark::State& state, std::int64_t count) {
  state.counters["calls_per_iteration"] = static_cast<double>(count);
  state.SetItemsProcessed(state.iterations() * count);
}

template <Workload Case, CallPath Path>
void Cpp(benchmark::State& state) {
  runtime::Isolate isolate;
  runtime::NativeContext context(isolate);
  auto target = Kernel<Case>();
  if constexpr (Path == CallPath::Indirect) {
    // 只在计时前隐藏函数指针目标，避免编译器把间接调用转回直接调用。
    benchmark::DoNotOptimize(target);
  }
  const auto invoke = [&](auto... arguments) {
    if constexpr (Path == CallPath::Direct) {
      return Kernel<Case>()(context, arguments...);
    } else {
      return target(context, arguments...);
    }
  };
  const auto count = state.range(0);
  const auto expected = Expected<Case>(count);
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    Sum<Case> sum = 0;
    for (std::int64_t index = 0; index < count; ++index) {
      if constexpr (Case == Workload::Empty) {
        invoke();
        sum += index + 1;
      } else if constexpr (Case == Workload::Integer) {
        sum += invoke(index);
      } else if constexpr (Case == Workload::Double) {
        sum += invoke(static_cast<double>(index));
      } else {
        sum += invoke(kBorrowedText);
      }
    }
    benchmark::DoNotOptimize(sum);
    if (sum != expected) {
      state.SkipWithError("Native C++ batch checksum mismatch");
      return;
    }
  }
  ReportCalls(state, count);
}

template <Workload Case>
bool IsExpected(const runtime::ExecutionResult& result, Sum<Case> expected) {
  if (!result.Succeeded()) {
    return false;
  }
  if constexpr (Case == Workload::Double) {
    return result.value.IsDouble() && result.value.AsDouble() == expected;
  } else {
    return result.value.IsInteger() && result.value.AsInteger() == expected;
  }
}

template <Workload Case, Execution Mode>
void Script(benchmark::State& state, native_call::Case problem, ScriptPath path) {
  runtime::Isolate isolate;
  if (!isolate.RegisterNative(runtime::NativeFunction::Bind<NativeEmpty>("callbench.empty")) ||
      !isolate.RegisterNative(runtime::NativeFunction::Bind<NativeInteger>("callbench.integer")) ||
      !isolate.RegisterNative(runtime::NativeFunction::Bind<NativeDouble>("callbench.double")) ||
      !isolate.RegisterNative(
          runtime::NativeFunction::Bind<NativeStringBorrow>("callbench.string_borrow"))) {
    state.SkipWithError("Cannot register native benchmark function");
    return;
  }
  const auto count = state.range(0);
  std::expected<std::unique_ptr<runtime::NativeInstance>, runtime::NativeInstantiationError>
      created;
  if constexpr (Mode == Execution::Jit) {
    auto compiler = compiler::jit::Compiler::Create();
    if (!compiler) {
      state.SkipWithError(frontend::DiagnosticsText(compiler.error()));
      return;
    }
    const auto compiled = compiler::jit::CompileFile(**compiler, native_call::kSource);
    if (!compiled.Succeeded()) {
      state.SkipWithError(frontend::DiagnosticsText(compiled.diagnostics));
      return;
    }
    created = runtime::NativeInstance::Instantiate(isolate, compiled.code.image);
  } else {
    created = runtime::NativeInstance::Instantiate(isolate, *native_call::kImage);
  }
  if (!created) {
    state.SkipWithError(created.error().message);
    return;
  }
  const auto& instance = *created;
  const auto entry =
      instance->FindFunction(path == ScriptPath::Native ? problem.native : problem.control);
  if (!entry) {
    state.SkipWithError("Missing native-call benchmark entry");
    return;
  }
  const auto function = *entry;
  const auto expected = Expected<Case>(count);
  if (!IsExpected<Case>(runtime::ExecuteNativeFunction(*instance, function), expected)) {
    state.SkipWithError("Native-call warm-up failed or checksum mismatched");
    return;
  }
  auto& heap = isolate.heap();
  heap.FinishCollection();
  heap.Collect();
  const auto baseline_objects = heap.object_count();
  const auto baseline_bytes = heap.allocated_bytes();
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    auto result = runtime::ExecuteNativeFunction(*instance, function);
    benchmark::DoNotOptimize(result);
    if (!IsExpected<Case>(result, expected)) {
      state.SkipWithError("Native-call execution failed or checksum mismatched");
      return;
    }
  }
  // 这里只报告堆存量；不能用前后差值冒充累计分配次数或宿主 malloc 次数。
  state.counters["baseline_heap_bytes"] = static_cast<double>(baseline_bytes);
  state.counters["end_heap_bytes"] = static_cast<double>(heap.allocated_bytes());
  state.counters["baseline_heap_objects"] = static_cast<double>(baseline_objects);
  state.counters["end_heap_objects"] = static_cast<double>(heap.object_count());
  ReportCalls(state, count);
}

template <Workload Case>
void Register(const native_call::Case& problem) {
  const auto suffix = std::string(problem.name);
  const auto count = problem.count;
  benchmark::RegisterBenchmark("native/cpp/" + suffix, Cpp<Case, CallPath::Direct>)
      ->Arg(count)
      ->UseRealTime();
  benchmark::RegisterBenchmark("native/cpp_indirect/" + suffix, Cpp<Case, CallPath::Indirect>)
      ->Arg(count)
      ->UseRealTime();
  benchmark::RegisterBenchmark("native/jit/" + suffix, Script<Case, Execution::Jit>, problem,
                               ScriptPath::Native)
      ->Arg(count)
      ->UseRealTime();
  benchmark::RegisterBenchmark("native/jit_control/" + suffix, Script<Case, Execution::Jit>,
                               problem, ScriptPath::Control)
      ->Arg(count)
      ->UseRealTime();
  benchmark::RegisterBenchmark("native/aot/" + suffix, Script<Case, Execution::Aot>, problem,
                               ScriptPath::Native)
      ->Arg(count)
      ->UseRealTime();
  benchmark::RegisterBenchmark("native/aot_control/" + suffix, Script<Case, Execution::Aot>,
                               problem, ScriptPath::Control)
      ->Arg(count)
      ->UseRealTime();
}

int RegisterNativeCalls() {
  for (const auto& problem : native_call::kCases) {
    switch (problem.workload) {
      case Workload::Empty:
        Register<Workload::Empty>(problem);
        break;
      case Workload::Integer:
        Register<Workload::Integer>(problem);
        break;
      case Workload::Double:
        Register<Workload::Double>(problem);
        break;
      case Workload::StringBorrow:
        Register<Workload::StringBorrow>(problem);
        break;
    }
  }
  return 0;
}

[[maybe_unused]] const int kRegistered = RegisterNativeCalls();

}  // namespace
}  // namespace vela::benchmarks
