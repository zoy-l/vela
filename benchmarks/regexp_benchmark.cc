#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "benchmark/benchmark.h"

#include "base/builtin.h"
#include "benchmarks/standard_value_benchmark.h"
#include "compiler/compiler.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/object.h"
#include "runtime/regexp.h"
#include "runtime/value.h"
#include "vm/vm.h"

namespace vela::benchmarks {
namespace {

using runtime::Value;

constexpr auto kCaptureChecksum = 8;
constexpr auto kAnswer = 42;

// 全局模式在计时前初始化并保留；临时对象核算从稳定的全局存储基线开始。
void SteadyRegExp(benchmark::State& state, std::string_view source, std::int64_t expected) {
  runtime::Isolate isolate;
  const auto compiled = compiler::CompileSource(source, isolate);
  if (!compiled.Succeeded()) {
    state.SkipWithError("RegExp benchmark compilation failed");
    return;
  }
  const auto entry = compiled.program.FindFunction("main");
  auto& heap = isolate.heap();
  vm::VirtualMachine machine(isolate);
  const auto warm = machine.Execute(entry);
  if (!warm.Succeeded() || !warm.value.IsInteger() || warm.value.AsInteger() != expected) {
    state.SkipWithError("RegExp warm-up returned an unexpected result");
    return;
  }
  heap.FinishCollection();
  heap.Collect();
  const auto baseline = heap.allocated_bytes();
  state.counters["persistent_bytes"] = static_cast<double>(baseline);
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    auto result = machine.Execute(entry);
    if (!result.Succeeded() || !result.value.IsInteger() || result.value.AsInteger() != expected) {
      state.SkipWithError("RegExp execution returned an unexpected result");
      return;
    }
    benchmark::DoNotOptimize(result.value);
  }
  heap.FinishCollection();
  heap.Collect();
  state.counters["retained_bytes"] = static_cast<double>(heap.allocated_bytes() - baseline);
  if (heap.allocated_bytes() != baseline) {
    state.SkipWithError("RegExp execution retained temporary heap values");
  }
}

std::array<Value, 4> CompileArguments(Value source) {
  return {source, Value::Boolean(false), Value::Boolean(true), Value::Boolean(false)};
}

void CompileRegExp(benchmark::State& state) {
  runtime::Heap heap;
  const auto source = Value::String(heap.AllocateString("(?<word>\\w+)-([0-9]+)"));
  runtime::RootRange root(heap, &source, 1);
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    const auto result =
        runtime::ExecuteRegExpBuiltin(Builtin::NewRegExp, CompileArguments(source), heap);
    if (!result) {
      state.SkipWithError(result.error().message);
      return;
    }
    benchmark::DoNotOptimize(result->AsObject());
  }
  heap.FinishCollection();
  heap.Collect();
}

void SearchRegExp(benchmark::State& state, Builtin builtin) {
  runtime::Heap heap;
  std::array<Value, 4> roots{};
  runtime::RootRange root(heap, roots.data(), roots.size());
  roots.at(0) = Value::String(heap.AllocateString("(?<word>中+)(x)?"));
  const auto compiled =
      runtime::ExecuteRegExpBuiltin(Builtin::NewRegExp, CompileArguments(roots.at(0)), heap);
  if (!compiled) {
    state.SkipWithError(compiled.error().message);
    return;
  }
  roots.at(0) = *compiled;
  const auto count = state.range(0);
  std::string text;
  for (std::int64_t index = 0; index < count; ++index) {
    text += "😀中 ";
  }
  roots.at(1) = Value::String(heap.AllocateString(text));
  const runtime::ArrayLayout layout{
      .element =
          {
              .kind = runtime::ValueKind::Object,
              .object_kind = runtime::ObjectKind::RegExpMatch,
          },
  };
  const std::array arguments{roots.at(0), roots.at(1), Value::Integer(0), Value::Integer(0)};
  heap.Collect();
  const auto baseline = heap.allocated_bytes();
  const auto objects = heap.object_count();
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    auto result = runtime::ExecuteRegExpBuiltin(
        builtin, std::span(arguments).first(BuiltinArity(builtin)), heap, &layout);
    if (!result) {
      state.SkipWithError(result.error().message);
      return;
    }
    if ((builtin == Builtin::RegExpHasMatch && !result->AsBoolean()) ||
        (builtin == Builtin::RegExpAllMatches &&
         std::cmp_not_equal(runtime::Heap::ArrayLength(*result->AsObject()).value_or(0), count))) {
      state.SkipWithError("Incorrect search result");
      return;
    }
    benchmark::DoNotOptimize(*result);
  }
  if (builtin == Builtin::RegExpHasMatch &&
      (heap.object_count() != objects || heap.allocated_bytes() != baseline)) {
    state.SkipWithError("hasMatch allocated GC objects or retained native work buffers");
  }
  heap.FinishCollection();
  heap.Collect();
  state.counters["subject_code_points"] = static_cast<double>(count * 3);
  state.counters["retained_bytes"] = static_cast<double>(heap.allocated_bytes() - baseline);
}

void CaptureRegExp(benchmark::State& state) {
  SteadyRegExp(state,
               "const re = RegExp('(?<word>中+)(x)?'); func main(): int { "
               "const found = re.firstMatch('😀中中'); if (found == null) { return 0; } "
               "const values = found.groups([1, 2]); return found.start + found.end + "
               "(found.namedGroup('word')?.length ?? 0) + values.length; }",
               kCaptureChecksum);
}

void LimitRegExp(benchmark::State& state) {
  SteadyRegExp(state,
               "const re = RegExp('(*NO_START_OPT)(*LIMIT_MATCH=100)(a+)+$'); "
               "func main(): int { try { re.hasMatch('aaaaaaaaaaaaaaaaaaaa!'); } "
               "catch (e: Error) { return e.code == 'RegExpLimitError' ? 42 : 0; } return 0; }",
               kAnswer);
}

BENCHMARK(CompileRegExp)->Name("regexp/compile");
BENCHMARK_CAPTURE(SearchRegExp, HasMatch, Builtin::RegExpHasMatch)
    ->Name("regexp/has_match")
    ->Arg(32)
    ->Arg(4096);
BENCHMARK_CAPTURE(SearchRegExp, FirstMatch, Builtin::RegExpFirstMatch)
    ->Name("regexp/first_match")
    ->Arg(32)
    ->Arg(4096);
BENCHMARK_CAPTURE(SearchRegExp, AllMatches, Builtin::RegExpAllMatches)
    ->Name("regexp/all_matches")
    ->Arg(32)
    ->Arg(4096);
BENCHMARK(CaptureRegExp)->Name("regexp/captures");
BENCHMARK(LimitRegExp)->Name("regexp/backtracking_limit");
BENCHMARK_CAPTURE(SteadyValue,
                  RegExpGc,
                  "func main(): int { let sum = 0; for (let i = 0; i < 100; i++) { "
                  "sum += RegExp('(?<x>中+)').allMatches('😀中 中中').length; } return sum; }",
                  200)
    ->Name("regexp/gc_pressure");

}  // namespace
}  // namespace vela::benchmarks
