#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include <benchmark/benchmark.h>

#include "base/opcode.h"

#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/frontend/typing/type_checker.h"
#include "compiler/ir/ir.h"
#include "compiler/ir/preparation.h"
#include "compiler/jit/compiler_protocol.h"

namespace vela::benchmarks {
namespace {

void PrepareContracts(benchmark::State& state, const std::string& source) {
  const frontend::SourceInput input{.path = "contracts.vela", .text = source};
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    auto result = ir::PrepareParsed(frontend::LoadSources(std::span(&input, 1), input.path));
    if (!result.Succeeded() || result.program.functions.empty()) {
      state.SkipWithError(frontend::DiagnosticsText(result.diagnostics));
      return;
    }
    benchmark::DoNotOptimize(result.program);
  }
}

void PrepareInheritedConstructors(benchmark::State& state) {
  std::string source;
  for (std::int64_t index = 0; index < state.range(0); ++index) {
    const auto suffix = std::to_string(index);
    source.append("class Base")
        .append(suffix)
        .append(" { value: int; extra: int = 2; } export class Child")
        .append(suffix)
        .append(" extends Base")
        .append(suffix)
        .append(" { constructor(super.value); }");
  }
  PrepareContracts(state, source);
}

void PreparePredicates(benchmark::State& state) {
  std::string source =
      "class Base { value: int = 42; } class First extends Base {} class Second extends Base {}";
  for (std::int64_t index = 0; index < state.range(0); ++index) {
    const auto suffix = std::to_string(index);
    source.append("func guard")
        .append(suffix)
        .append(
            "(value: dynamic, flag: bool): bool { if (flag && value is First) { return true; }"
            "if (value is Second) { return true; } if (value is First) { return true; }"
            "if (value is Second) { return true; } return false; }");
    source.append("export func select")
        .append(suffix)
        .append("(values: dynamic[]): Base[] { return values.filter(value => guard")
        .append(suffix)
        .append("(value, true)); }");
  }
  PrepareContracts(state, source);
}

void PrepareCheckedStructs(benchmark::State& state, const std::string& source) {
  const frontend::SourceInput input{.path = "assignments.vela", .text = source};
  auto parsed = frontend::LoadSources(std::span(&input, 1), input.path);
  const auto checked = frontend::TypeChecker().Check(parsed.program);
  if (!parsed.diagnostics.empty() || !checked.Succeeded()) {
    state.SkipWithError("cannot check struct assignment workload");
    return;
  }
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    auto result = ir::PrepareProgram(parsed.program, checked.model);
    const auto function =
        std::ranges::find(result.program.functions, "update", &ir::Function::name);
    if (!result.Succeeded() || function == result.program.functions.end() ||
        std::ranges::count_if(function->instructions, [](const auto& instruction) {
          return instruction.opcode == Opcode::SetField;
        }) < state.range(0)) {
      state.SkipWithError("struct assignment workload did not preserve writes");
      return;
    }
    state.counters["instructions"] = static_cast<double>(function->instructions.size());
    state.counters["field_reads"] = static_cast<double>(
        std::ranges::count(function->instructions, Opcode::GetField, &ir::Instruction::opcode));
    state.counters["calls"] = static_cast<double>(
        std::ranges::count(function->instructions, Opcode::Call, &ir::Instruction::opcode));
    benchmark::DoNotOptimize(result.program);
  }
}

void PrepareStructAssignments(benchmark::State& state, bool nested) {
  std::string source = "struct Point { x: int; y: int; } ";
  source += nested ? "let calls = 0; func supply(value: int): int { calls++; return value; } "
                     "struct Body { point: Point; other: int; } "
                     "export func update(value: int): int {"
                     "let body = Body(point: Point(x: 0, y: 1), other: 2);"
                   : "export func update(value: int): int { let point = Point(x: 0, y: 1);";
  for (std::int64_t index = 0; index < state.range(0); ++index) {
    source += nested ? "body.point.x = supply(value);" : "point.x = value;";
  }
  source += nested ? "return body.point.x + body.point.y; }" : "return point.x + point.y; }";
  PrepareCheckedStructs(state, source);
}

void PrepareStructNullDefaults(benchmark::State& state) {
  std::string source =
      "func initial(): int { return 1; } "
      "struct Entry { value: int = initial(); text: string? = null; } "
      "export func update(value: int): int { let total = 0;";
  for (std::int64_t index = 0; index < state.range(0); ++index) {
    source += "{ const entry = Entry(value: value); total += entry.value; }";
  }
  source += "return total; }";
  PrepareCheckedStructs(state, source);
}

void CompileStructProtocol(benchmark::State& state) {
  std::string source = "struct Layer0 { value: int; } ";
  for (std::int64_t depth = 1; depth <= state.range(0); ++depth) {
    source += "struct Layer" + std::to_string(depth) + " { left: Layer" +
              std::to_string(depth - 1) + "; right: Layer" + std::to_string(depth - 1) + "; } ";
  }
  constexpr std::int64_t kFunctionCount = 32;
  for (std::int64_t index = 0; index < kFunctionCount; ++index) {
    source += "export func copy" + std::to_string(index) + "(value: Layer" +
              std::to_string(state.range(0)) + "): Layer" + std::to_string(state.range(0)) +
              " { return value; } ";
  }
  const frontend::SourceInput input{.path = "protocol.vela", .text = source};
  const auto prepared = ir::PrepareParsed(frontend::LoadSources(std::span(&input, 1), input.path));
  if (!prepared.Succeeded()) {
    state.SkipWithError(frontend::DiagnosticsText(prepared.diagnostics));
    return;
  }
  std::size_t bytes = 0;
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    const auto encoded = compiler::jit::EncodeProgram(prepared.program);
    auto decoded = compiler::jit::DecodeProgram(encoded);
    if (!decoded || decoded->functions.size() != prepared.program.functions.size()) {
      state.SkipWithError("struct protocol did not preserve functions");
      return;
    }
    bytes = encoded.size();
    benchmark::DoNotOptimize(*decoded);
  }
  state.counters["encoded_bytes"] = static_cast<double>(bytes);
  state.counters["instruction_bytes"] = sizeof(ir::Instruction);
}

BENCHMARK(PrepareInheritedConstructors)
    ->Name("compiler/contracts/inherited_constructors")
    ->Arg(64)
    ->UseRealTime();
BENCHMARK(PreparePredicates)->Name("compiler/contracts/predicates")->Arg(32)->UseRealTime();
BENCHMARK_CAPTURE(PrepareStructAssignments, Flat, false)
    ->Name("compiler/struct_assignments")
    ->Arg(128)
    ->UseRealTime();
BENCHMARK_CAPTURE(PrepareStructAssignments, Nested, true)
    ->Name("compiler/nested_struct_assignments")
    ->Arg(128)
    ->UseRealTime();
BENCHMARK(PrepareStructNullDefaults)
    ->Name("compiler/struct_null_defaults")
    ->Arg(128)
    ->UseRealTime();
BENCHMARK(CompileStructProtocol)->Name("compiler/struct_protocol")->Arg(6)->UseRealTime();

}  // namespace
}  // namespace vela::benchmarks
