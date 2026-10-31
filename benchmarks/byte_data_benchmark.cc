#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "benchmark/benchmark.h"

#include "benchmarks/standard_value_benchmark.h"
#include "runtime/heap/heap.h"
#include "runtime/value/value.h"

namespace vela::benchmarks {
namespace {
constexpr std::int64_t kAnswer = 42;
constexpr std::int64_t kIntegerLoopSum = 499500;

void AllocateBytes(benchmark::State& state) {
  const auto size = state.range(0);
  state.counters["byte_length"] = static_cast<double>(size);
  SteadyValue(state, "func main(): int { return ByteArray(" + std::to_string(size) + ").length; }",
              size);
}

void CopyBytes(benchmark::State& state) {
  const auto size = state.range(0);
  const auto text = std::to_string(size);
  state.counters["copied_bytes"] = static_cast<double>(size);
  SteadyValue(state,
              "func main(): int { const bytes = ByteArray(" + text + "); bytes[" + text +
                  "-1] = 42; const copy = bytes.slice(); return copy[" + text + "-1]; }",
              kAnswer);
}

void ByteAccess(benchmark::State& state) {
  const auto count = state.range(0);
  const auto size = std::to_string(count);
  constexpr std::int64_t kByteValues = 256;
  const auto expected =
      count < kByteValues ? count * (count - 1) / 2 : count * (kByteValues - 1) / 2;
  SteadyValue(state,
              "func main(): int { const bytes = ByteArray(" + size +
                  "); let sum = 0; "
                  "for (let i = 0; i < " +
                  size + "; i++) { bytes[i] = i & 255; sum += bytes[i]; } return sum; }",
              expected);
}

void TypedAccess(benchmark::State& state, std::string_view endian) {
  const auto offset = state.range(0);
  state.counters["access_offset"] = static_cast<double>(offset);
  const auto order = std::string(endian);
  SteadyValue(state,
              "func main(): int { const data = ByteData.view(ByteArray(16), offset: " +
                  std::to_string(offset) +
                  "); let sum = 0; "
                  "for (let i = 0; i < 1000; i++) { data.setInt64(0, i, endian: Endian." +
                  order + "); sum += data.getInt64(0, endian: Endian." + order +
                  "); } return sum; }",
              kIntegerLoopSum);
}

// 所有者及其清零在计时外，单独观察 O(1) 视图构造与 GC。
void RuntimeView(benchmark::State& state) {
  runtime::Heap heap;
  const auto length = static_cast<std::size_t>(state.range(0));
  auto* owner = heap.AllocateByteArray(length);
  if (owner == nullptr) {
    state.SkipWithError("ByteArray owner allocation failed");
    return;
  }
  const auto value = runtime::Value::Object(owner);
  runtime::RootRange root(heap, &value, 1);
  heap.Collect();
  const auto baseline = heap.allocated_bytes();
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    auto* view =
        heap.AllocateByteData(*owner, {.offset = 0, .length = static_cast<std::uint32_t>(length)});
    if (view == nullptr || runtime::Heap::ByteDataBytes(*view).size() != length) {
      state.SkipWithError("View allocation or length check failed");
      return;
    }
    benchmark::DoNotOptimize(view);
  }
  heap.FinishCollection();
  heap.Collect();
  state.counters["owner_bytes"] = static_cast<double>(baseline);
  state.counters["retained_bytes"] = static_cast<double>(heap.allocated_bytes() - baseline);
  if (heap.allocated_bytes() != baseline) {
    state.SkipWithError("View collection retained temporary objects");
  }
}

BENCHMARK(AllocateBytes)->Name("byte/allocate_zero")->Arg(0)->Arg(64)->Arg(4096)->Arg(65536);
BENCHMARK(CopyBytes)->Name("byte/copy")->Arg(64)->Arg(4096)->Arg(65536);
BENCHMARK(ByteAccess)->Name("byte/index")->Arg(128)->Arg(4096);
BENCHMARK_CAPTURE(TypedAccess, Big, "big")->Name("byte/typed_big")->Arg(0)->Arg(1);
BENCHMARK_CAPTURE(TypedAccess, Little, "little")->Name("byte/typed_little")->Arg(0)->Arg(1);
BENCHMARK_CAPTURE(TypedAccess, Host, "host")->Name("byte/typed_host")->Arg(0)->Arg(1);
BENCHMARK(RuntimeView)->Name("byte/runtime_view")->Arg(64)->Arg(4096)->Arg(65536);
BENCHMARK_CAPTURE(SteadyValue,
                  ByteGc,
                  "func main(): int { let sum = 0; for (let i = 0; i < 1000; i++) { "
                  "const view = ByteData.view(ByteArray(64)); view.setUint8(0, 1); sum += "
                  "view.getUint8(0); } return sum; }",
                  1000)
    ->Name("byte/gc_pressure");

}  // namespace
}  // namespace vela::benchmarks
