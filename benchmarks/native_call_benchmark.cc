#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

#include "benchmark/benchmark.h"

#include "benchmarks/native_call_kernels.h"
#include "benchmarks/source.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_function.h"
#include "vm/vm.h"

namespace vela::benchmarks {
namespace {

enum class Workload : std::uint8_t { Empty, Integer, Double, StringBorrow };
enum class CallPath : std::uint8_t { Direct, Indirect };
enum class VmPath : std::uint8_t { Native, Control };

constexpr std::string_view kBorrowedText = "vela-native";
constexpr std::int64_t kBorrowedResult = 129;
constexpr double kArithmeticSeriesDivisor = 2.0;
// 两种批量大小用于观察宿主进入 VM 的固定成本能否被摊薄。
constexpr std::array<std::int64_t, 2> kBatchSizes{64, 4096};

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
std::string Source(std::int64_t count, VmPath path) {
  std::string declaration;
  std::string operation;
  if constexpr (Case == Workload::Empty) {
    declaration = "external func host(): void;";
    operation = path == VmPath::Native ? "host(); sum += index + 1;" : "sum += index + 1;";
  } else if constexpr (Case == Workload::Integer) {
    declaration = "external func host(value: int): int;";
    operation = path == VmPath::Native ? "sum += host(index);" : "sum += index + 1;";
  } else if constexpr (Case == Workload::Double) {
    declaration = "external func host(value: double): double;";
    operation = path == VmPath::Native
                    ? "sum += host(index.toDouble());"
                    : "sum += index.toDouble() + " + std::to_string(kNativeDoubleIncrement) + ";";
  } else {
    declaration = "external func host(text: string): int;";
    operation = path == VmPath::Native ? "sum += host('" + std::string(kBorrowedText) + "');"
                                       : "sum += " + std::to_string(kBorrowedResult) + ";";
  }
  const auto* type = Case == Workload::Double ? "double" : "int";
  const auto* zero = Case == Workload::Double ? "0.0" : "0";
  return declaration + "func main(): " + type + " { let sum = " + zero +
         "; for (let index = 0; index < " + std::to_string(count) + "; index++) { " + operation +
         " } return sum; }";
}

template <Workload Case>
bool IsExpected(const vm::ExecutionResult& result, Sum<Case> expected) {
  if (!result.Succeeded()) {
    return false;
  }
  if constexpr (Case == Workload::Double) {
    return result.value.IsDouble() && result.value.AsDouble() == expected;
  } else {
    return result.value.IsInteger() && result.value.AsInteger() == expected;
  }
}

template <Workload Case>
void Vm(benchmark::State& state, VmPath path) {
  runtime::Isolate isolate;
  if (!isolate.RegisterNative(runtime::NativeFunction::Bind<Kernel<Case>()>("host"))) {
    state.SkipWithError("Cannot register native benchmark function");
    return;
  }
  const auto count = state.range(0);
  const auto entry = CompileBenchmarkEntry(state, Source<Case>(count, path), isolate);
  if (!entry) {
    return;
  }
  const auto& function = *entry;
  vm::VirtualMachine machine(isolate);
  const auto expected = Expected<Case>(count);
  if (!IsExpected<Case>(machine.Execute(function), expected)) {
    state.SkipWithError("Native VM warm-up failed or checksum mismatched");
    return;
  }
  auto& heap = isolate.heap();
  heap.FinishCollection();
  heap.Collect();
  const auto baseline_objects = heap.object_count();
  const auto baseline_bytes = heap.allocated_bytes();
  const auto register_bytes = machine.register_bytes();
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    auto result = machine.Execute(function);
    benchmark::DoNotOptimize(result);
    if (!IsExpected<Case>(result, expected)) {
      state.SkipWithError("Native VM execution failed or checksum mismatched");
      return;
    }
  }
  // 这里只报告堆存量；不能用前后差值冒充累计分配次数或宿主 malloc 次数。
  state.counters["baseline_heap_bytes"] = static_cast<double>(baseline_bytes);
  state.counters["end_heap_bytes"] = static_cast<double>(heap.allocated_bytes());
  state.counters["baseline_heap_objects"] = static_cast<double>(baseline_objects);
  state.counters["end_heap_objects"] = static_cast<double>(heap.object_count());
  state.counters["register_bytes"] = static_cast<double>(register_bytes);
  if (machine.register_bytes() != register_bytes) {
    state.SkipWithError("Native benchmark register capacity grew after warm-up");
    return;
  }
  ReportCalls(state, count);
}

template <Workload Case>
void Register(std::string_view name) {
  const auto suffix = std::string(name);
  for (const auto count : kBatchSizes) {
    benchmark::RegisterBenchmark("native/cpp/" + suffix, Cpp<Case, CallPath::Direct>)
        ->Arg(count)
        ->UseRealTime();
    benchmark::RegisterBenchmark("native/cpp_indirect/" + suffix, Cpp<Case, CallPath::Indirect>)
        ->Arg(count)
        ->UseRealTime();
    benchmark::RegisterBenchmark("native/vm/" + suffix, Vm<Case>, VmPath::Native)
        ->Arg(count)
        ->UseRealTime();
    benchmark::RegisterBenchmark("native/vm_control/" + suffix, Vm<Case>, VmPath::Control)
        ->Arg(count)
        ->UseRealTime();
  }
}

int RegisterNativeCalls() {
  Register<Workload::Empty>("empty");
  Register<Workload::Integer>("integer");
  Register<Workload::Double>("double");
  Register<Workload::StringBorrow>("string_borrow");
  return 0;
}

[[maybe_unused]] const int kRegistered = RegisterNativeCalls();

}  // namespace
}  // namespace vela::benchmarks
