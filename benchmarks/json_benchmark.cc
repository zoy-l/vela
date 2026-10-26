#include <cstdint>
#include <string>
#include <string_view>

#include "benchmark/benchmark.h"

#include "benchmarks/standard_value_benchmark.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/json.h"
#include "runtime/rooted_value.h"
#include "runtime/string.h"

namespace vela::benchmarks {
namespace {

constexpr std::int64_t kAnswer = 42;
constexpr std::int64_t kOperationCount = 1000;

std::string Payload(std::int64_t count) {
  std::string text = "[";
  for (std::int64_t index = 0; index < count; ++index) {
    if (index != 0) {
      text += ',';
    }
    text += R"({"id":7,"text":"Vela😀","items":[true,null,1.25]})";
  }
  return text + ']';
}

void DecodeJson(benchmark::State& state) {
  runtime::Isolate isolate;
  const auto count = state.range(0);
  const auto source = Payload(count);
  auto& heap = isolate.heap();
  {
    const auto warm = runtime::ParseJson(source, isolate);
    if (!warm) {
      state.SkipWithError("JSON decoding warm-up failed");
      return;
    }
    state.counters["first_call_gc_bytes"] = static_cast<double>(heap.allocated_bytes());
    state.counters["first_call_objects"] = static_cast<double>(heap.object_count());
  }
  heap.FinishCollection();
  heap.Collect();
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    const auto value = runtime::ParseJson(source, isolate);
    if (!value || runtime::Heap::ArrayLength(*value->AsObject()) != count) {
      state.SkipWithError("JSON decoding returned an unexpected value");
      return;
    }
    benchmark::DoNotOptimize(*value);
  }
  heap.FinishCollection();
  heap.Collect();
  state.counters["retained_bytes"] = static_cast<double>(heap.allocated_bytes());
  if (heap.allocated_bytes() != 0) {
    state.SkipWithError("JSON decoding retained temporary heap values");
  }
  state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(source.size()));
}

void EncodeJson(benchmark::State& state) {
  runtime::Isolate isolate;
  const auto source = Payload(state.range(0));
  const auto input = runtime::ParseJson(source, isolate);
  if (!input) {
    state.SkipWithError("JSON fixture decoding failed");
    return;
  }
  const runtime::RootedValue root(isolate, *input);
  auto& heap = isolate.heap();
  heap.FinishCollection();
  heap.Collect();
  const auto baseline = heap.allocated_bytes();
  {
    const auto warm = runtime::JsonEncoder::Create(isolate, root.value());
    if (!warm) {
      state.SkipWithError("JSON encoding warm-up allocation failed");
      return;
    }
    const auto encoded = warm->Advance();
    if (!encoded || encoded->action != runtime::JsonEncodeAction::Complete) {
      state.SkipWithError("JSON encoding warm-up failed");
      return;
    }
    state.counters["first_call_gc_bytes"] = static_cast<double>(heap.allocated_bytes() - baseline);
  }
  heap.FinishCollection();
  heap.Collect();
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    const auto encoder = runtime::JsonEncoder::Create(isolate, root.value());
    if (!encoder) {
      state.SkipWithError("JSON encoder allocation failed");
      return;
    }
    const auto result = encoder->Advance();
    if (!result || result->action != runtime::JsonEncodeAction::Complete ||
        !result->value.IsString() || result->value.AsString()->value() != source) {
      state.SkipWithError("JSON encoding returned an unexpected value");
      return;
    }
    benchmark::DoNotOptimize(result->value);
  }
  heap.FinishCollection();
  heap.Collect();
  state.counters["input_heap_bytes"] = static_cast<double>(baseline);
  state.counters["retained_bytes"] = static_cast<double>(heap.allocated_bytes() - baseline);
  if (heap.allocated_bytes() != baseline) {
    state.SkipWithError("JSON encoding retained temporary heap values");
  }
  state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(source.size()));
}

void ConvertObject(benchmark::State& state) {
  SteadyValue(state, R"(class Payload {
  public toJson(): int[] { return [20, 22]; }
}
func main(): int { return Json.stringify(Payload()) == '[20,22]' ? 42 : 0; })",
              kAnswer);
}

void DynamicMembers(benchmark::State& state) {
  SteadyValue(state, R"(class Counter {
  public param count: int = 0;
  public next(): int { this.count += 1; return this.count; }
}
func main(): int {
  const receiver: dynamic = Counter();
  let result = 0;
  for (let index = 0; index < 1000; index += 1) { result = receiver.next(); }
  return result;
})",
              kOperationCount);
}

void CheckedBoundaries(benchmark::State& state) {
  SteadyValue(state, R"(func main(): int {
  let value: dynamic = 0;
  for (let index = 0; index < 1000; index += 1) {
    const integer: int = value;
    value = integer + 1;
  }
  const result: int = value;
  return result;
})",
              kOperationCount);
}

BENCHMARK(DecodeJson)->Name("json/decode")->Arg(1)->Arg(64)->Arg(4096);
BENCHMARK(EncodeJson)->Name("json/encode")->Arg(1)->Arg(64)->Arg(4096);
BENCHMARK(ConvertObject)->Name("json/to_json");
BENCHMARK(DynamicMembers)->Name("dynamic/member_calls");
BENCHMARK(CheckedBoundaries)->Name("dynamic/typed_boundaries");

}  // namespace
}  // namespace vela::benchmarks
