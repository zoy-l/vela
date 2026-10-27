#include "benchmarks/standard_value_benchmark.h"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "benchmark/benchmark.h"

#include "compiler/compiler.h"
#include "runtime/isolate.h"
#include "vm/vm.h"

namespace vela::benchmarks {

void SteadyValue(benchmark::State& state, std::string_view source, std::int64_t expected) {
  runtime::Isolate isolate;
  const auto compiled = compiler::CompileSource(source, isolate);
  if (!compiled.Succeeded()) {
    std::string message = "Standard API benchmark compilation failed";
    for (const auto& diagnostic : compiled.diagnostics) {
      message += "\n" + diagnostic.message;
    }
    state.SkipWithError(message);
    return;
  }
  const auto entry = compiled.program.FindFunction("main");
  auto& heap = isolate.heap();
  heap.Collect();
  const auto baseline = heap.allocated_bytes();
  vm::VirtualMachine machine(isolate);
  const auto warm = machine.Execute(entry);
  if (!warm.Succeeded() || !warm.value.IsInteger() || warm.value.AsInteger() != expected) {
    state.SkipWithError("Standard API warm-up returned an unexpected result");
    return;
  }
  state.counters["first_call_gc_bytes"] = static_cast<double>(heap.allocated_bytes() - baseline);
  state.counters["register_bytes"] = static_cast<double>(machine.register_bytes());
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    auto result = machine.Execute(entry);
    if (!result.Succeeded() || !result.value.IsInteger() || result.value.AsInteger() != expected) {
      state.SkipWithError("Standard API execution returned an unexpected result");
      return;
    }
    benchmark::DoNotOptimize(result.value);
  }
  heap.FinishCollection();
  heap.Collect();
  state.counters["retained_bytes"] = static_cast<double>(heap.allocated_bytes() - baseline);
  if (heap.allocated_bytes() != baseline) {
    state.SkipWithError("Standard API execution retained temporary heap values");
  }
}

namespace {

enum class CaseWorkload : std::uint8_t {
  UnchangedAscii,
  ChangingAscii,
  Unicode,
  Expansion,
  Context,
};

void ConvertCase(benchmark::State& state, CaseWorkload workload) {
  const auto count = state.range(0);
  std::string_view part;
  std::int64_t output_width = 0;
  const auto* method = "toLowerCase";
  switch (workload) {
    case CaseWorkload::UnchangedAscii:
      part = "vela";
      output_width = 4;
      break;
    case CaseWorkload::ChangingAscii:
      part = "Vela";
      output_width = 4;
      break;
    case CaseWorkload::Unicode:
      part = "ÉΩ";
      output_width = 4;
      break;
    case CaseWorkload::Expansion:
      part = "ΐ";
      output_width = static_cast<std::int64_t>(std::string_view("Ι\xcc\x88\xcc\x81").size());
      method = "toUpperCase";
      break;
    case CaseWorkload::Context:
      part = "AΣ\xcc\x81";
      output_width = static_cast<std::int64_t>(part.size());
      break;
  }
  std::string input;
  for (std::int64_t index = 0; index < count; ++index) {
    input += part;
  }
  const auto output_bytes = count * output_width;
  state.counters["input_bytes"] = static_cast<double>(input.size());
  state.counters["output_bytes"] = static_cast<double>(output_bytes);
  const auto source = "func main(): int { return '" + input + "'." + method + "().byteLength; }";
  SteadyValue(state, source, output_bytes);
}

BENCHMARK_CAPTURE(ConvertCase, UnchangedAscii, CaseWorkload::UnchangedAscii)
    ->Name("case/unchanged_ascii")
    ->Arg(8)
    ->Arg(256)
    ->Arg(4096);
BENCHMARK_CAPTURE(ConvertCase, ChangingAscii, CaseWorkload::ChangingAscii)
    ->Name("case/changing_ascii")
    ->Arg(8)
    ->Arg(256)
    ->Arg(4096);
BENCHMARK_CAPTURE(ConvertCase, Unicode, CaseWorkload::Unicode)
    ->Name("case/unicode")
    ->Arg(8)
    ->Arg(256)
    ->Arg(4096);
BENCHMARK_CAPTURE(ConvertCase, Expansion, CaseWorkload::Expansion)
    ->Name("case/expansion")
    ->Arg(8)
    ->Arg(256)
    ->Arg(4096);
BENCHMARK_CAPTURE(ConvertCase, Context, CaseWorkload::Context)
    ->Name("case/context")
    ->Arg(8)
    ->Arg(256)
    ->Arg(4096);

BENCHMARK_CAPTURE(SteadyValue,
                  IntDecimal,
                  "func main(): int { return int.parse('9223372036854775807'); }",
                  std::numeric_limits<std::int64_t>::max())
    ->Name("parse/int_decimal");
BENCHMARK_CAPTURE(SteadyValue,
                  IntHex,
                  "func main(): int { return int.parse('2a', radix: 16); }",
                  42)
    ->Name("parse/int_hex");
BENCHMARK_CAPTURE(SteadyValue,
                  Double,
                  "func main(): int { return double.parse('1.25e2') == 125.0 ? 42 : 0; }",
                  42)
    ->Name("parse/double");
BENCHMARK_CAPTURE(SteadyValue,
                  Invalid,
                  "func main(): int { return int.tryParse('invalid') ?? 42; }",
                  42)
    ->Name("parse/invalid_try");
BENCHMARK_CAPTURE(SteadyValue,
                  DateParse,
                  "func main(): int { return Date.parse('1970-01-01T08:00:00.042+08:00'); }",
                  42)
    ->Name("date/parse");
BENCHMARK_CAPTURE(SteadyValue,
                  DateFormat,
                  "func main(): int { return Date.toISOString(42).byteLength; }",
                  24)
    ->Name("date/format");
BENCHMARK_CAPTURE(SteadyValue,
                  FromUTC,
                  "func main(): int { return Date.fromUTC(1970, 1, 1, millisecond: 42); }",
                  42)
    ->Name("date/from_utc");
BENCHMARK_CAPTURE(SteadyValue, ToUTC, "func main(): int { return Date.toUTC(42).millisecond; }", 42)
    ->Name("date/to_utc");
BENCHMARK_CAPTURE(SteadyValue,
                  WallClock,
                  "func main(): int { return Date.now() > 0 ? 42 : 0; }",
                  42)
    ->Name("clock/wall");
BENCHMARK_CAPTURE(SteadyValue,
                  MonotonicClock,
                  "func main(): int { return performance.now() >= 0.0 ? 42 : 0; }",
                  42)
    ->Name("clock/monotonic");

}  // namespace
}  // namespace vela::benchmarks
