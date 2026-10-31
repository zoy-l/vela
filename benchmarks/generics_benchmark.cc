#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <benchmark/benchmark.h>

#include "base/declaration.h"
#include "base/native_program_mode.h"
#include "benchmarks/process_memory.h"
#include "compiler/aot/native_compiler.h"
#include "compiler/bytecode/compiler.h"
#include "compiler/development/source_session.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/syntax/ast_clone.h"
#include "compiler/frontend/syntax/parser.h"
#include "compiler/frontend/typing/type_checker.h"
#include "compiler/ir/preparation.h"
#include "compiler/jit/compiler.h"
#include "runtime/isolate.h"
#include "runtime/native_dispatch.h"
#include "runtime/native_function.h"
#include "runtime/value.h"
#include "vm/bytecode.h"
#include "vm/program.h"
#include "vm/vm.h"

namespace vela::benchmarks {
namespace {

enum class GenericVariant : std::uint8_t { Concrete, Unbounded, Bounded };
enum class GenericShape : std::uint8_t { Scalar, Projection };
enum class GenericPhase : std::uint8_t { Definitions, Applications };

constexpr std::int64_t kGenericIterations = 128;
constexpr std::int64_t kSmallGenericCount = 4;
constexpr std::int64_t kMediumGenericCount = 16;
constexpr std::int64_t kLargeGenericCount = 32;
constexpr std::string_view kGenericPath = "generics.vela";

std::int64_t GenericInput() noexcept {
  return 1;
}

std::string GenericSource(GenericVariant variant,
                          GenericShape shape,
                          std::int64_t count,
                          GenericPhase phase) {
  const bool projection = shape == GenericShape::Projection;
  std::string source = projection ? "interface Reader<T> { read(): int; } "
                                    "class Parent<T> implements Reader<T> { value: int; "
                                    "read(): int { return this.value; } } "
                                  : "";
  std::string declarations;
  std::string body;
  source += "external func input(): int; ";
  for (std::int64_t index = 0; index < count; ++index) {
    const auto suffix = std::to_string(index);
    const auto type = projection ? "Item" + suffix : "int";
    if (projection) {
      source.append("class ")
          .append(type)
          .append(" extends Parent<")
          .append(type)
          .append("> { constructor(value: int) { super(value: value); } } ");
    }
    const auto parameter = variant == GenericVariant::Concrete ? type : "T";
    source.append("func identity").append(suffix);
    if (variant != GenericVariant::Concrete) {
      if (variant == GenericVariant::Bounded) {
        source += projection ? "<T extends Reader<T>>" : "<T extends int>";
      } else {
        source += "<T>";
      }
    }
    source.append("(value: ")
        .append(parameter)
        .append("): ")
        .append(parameter)
        .append(" { return value; } ");
    if (projection) {
      declarations.append("const item")
          .append(suffix)
          .append(" = ")
          .append(type)
          .append("(value: seed + ")
          .append(suffix)
          .append("); ");
      body.append("sum += identity")
          .append(suffix)
          .append("(item")
          .append(suffix)
          .append(").read() + index; ");
    } else {
      body.append("sum += identity")
          .append(suffix)
          .append("(seed + index + ")
          .append(suffix)
          .append("); ");
    }
  }
  source += "export func main(): int { const seed = input(); ";
  if (phase == GenericPhase::Definitions) {
    return source + "return seed; }";
  }
  return source + declarations + "let sum = 0; for (let index = 0; index < " +
         std::to_string(kGenericIterations) + "; index++) { " + body + "} return sum; }";
}

frontend::ParseResult LoadGenericSource(std::string_view source) {
  const frontend::SourceInput input{.path = kGenericPath, .text = source};
  return frontend::LoadSources(std::span(&input, 1), input.path);
}

std::int64_t ExpectedGenericResult(std::int64_t count) {
  const auto iterations = kGenericIterations;
  return (count * (iterations + ((iterations * (iterations - 1)) / 2))) +
         ((iterations * count * (count - 1)) / 2);
}

void ReportGenericPeak(benchmark::State& state) {
  const auto peak = PeakResidentBytes();
  if (!peak) {
    state.SkipWithError(peak.error());
    return;
  }
  state.counters["process_peak_resident_bytes"] = static_cast<double>(*peak);
}

void ReportGenericApplications(benchmark::State& state, const frontend::Program& program) {
  std::size_t functions = 0;
  std::size_t types = 0;
  for (const auto& [name, origin] : program.origins) {
    if (origin.arguments.empty()) {
      continue;
    }
    if (origin.kind == DeclarationKind::Function) {
      ++functions;
    } else {
      ++types;
    }
  }
  state.counters["function_specializations"] = static_cast<double>(functions);
  state.counters["type_specializations"] = static_cast<double>(types);
}

void GenericFrontend(benchmark::State& state,
                     GenericVariant variant,
                     GenericShape shape,
                     GenericPhase phase) {
  const auto source = GenericSource(variant, shape, state.range(0), phase);
  const auto parsed = LoadGenericSource(source);
  if (!parsed.diagnostics.empty()) {
    state.SkipWithError(frontend::DiagnosticsText(parsed.diagnostics));
    return;
  }
  auto sample = frontend::CloneProgram(parsed.program, {});
  const auto validated = frontend::TypeChecker().Check(sample);
  if (!validated.Succeeded()) {
    state.SkipWithError(frontend::DiagnosticsText(validated.diagnostics));
    return;
  }
  ReportGenericApplications(state, sample);
  for (auto iteration : state) {
    state.PauseTiming();
    auto program = frontend::CloneProgram(parsed.program, {});
    state.ResumeTiming();
    auto checked = frontend::TypeChecker().Check(program);
    if (!checked.Succeeded()) {
      state.SkipWithError(frontend::DiagnosticsText(checked.diagnostics));
      break;
    }
    benchmark::DoNotOptimize(checked);
  }
  state.counters["source_bytes"] = static_cast<double>(source.size());
  ReportGenericPeak(state);
}

void GenericVm(benchmark::State& state, GenericVariant variant, GenericShape shape) {
  const auto source = GenericSource(variant, shape, state.range(0), GenericPhase::Applications);
  runtime::Isolate isolate;
  if (!isolate.RegisterNative(runtime::NativeFunction::Bind<GenericInput>("input"))) {
    state.SkipWithError("generic input registration failed");
    return;
  }
  const auto compiled = compiler::CompileSource(source, isolate);
  if (!compiled.Succeeded()) {
    state.SkipWithError(frontend::DiagnosticsText(compiled.diagnostics));
    return;
  }
  const auto entry = compiled.program.FindFunction("main");
  const auto expected = ExpectedGenericResult(state.range(0));
  vm::VirtualMachine machine(isolate);
  std::size_t code_bytes = 0;
  for (const auto& function : compiled.program.functions()) {
    code_bytes += function.code.size() * sizeof(vm::Instruction);
  }
  for (auto iteration : state) {
    auto result = machine.Execute(compiled.program, entry.index());
    if (!result.Succeeded() || !result.value.IsInteger() || result.value.AsInteger() != expected) {
      state.SkipWithError("generic VM checksum mismatch");
      break;
    }
    benchmark::DoNotOptimize(result);
  }
  state.counters["bytecode_bytes"] = static_cast<double>(code_bytes);
  state.counters["register_bytes"] = static_cast<double>(machine.register_bytes());
  state.counters["result"] = static_cast<double>(expected);
  ReportGenericPeak(state);
}

void GenericNative(benchmark::State& state,
                   GenericVariant variant,
                   GenericShape shape,
                   NativeCodeTier tier) {
  const auto source = GenericSource(variant, shape, state.range(0), GenericPhase::Applications);
  auto prepared = ir::PrepareParsed(LoadGenericSource(source));
  if (!prepared.Succeeded()) {
    state.SkipWithError(frontend::DiagnosticsText(prepared.diagnostics));
    return;
  }
  const auto artifact = compiler::aot::Compile(prepared.program, {.exports = {"main"}});
  if (!artifact) {
    state.SkipWithError(frontend::DiagnosticsText(artifact.error()));
    return;
  }
  auto compiler = compiler::jit::Compiler::Create();
  if (!compiler) {
    state.SkipWithError(frontend::DiagnosticsText(compiler.error()));
    return;
  }
  compiler::SourceSession session{std::string(kGenericPath)};
  const auto candidate = session.Prepare({
      .base_revision = 0,
      .changes = {{.path = std::string(kGenericPath), .text = source}},
      .added_roots = {},
  });
  if (!candidate) {
    state.SkipWithError(frontend::DiagnosticsText(candidate.error().diagnostics));
    return;
  }
  const auto compiled = (*compiler)->Compile(candidate->snapshot().program, {.tier = tier});
  if (!compiled) {
    state.SkipWithError(frontend::DiagnosticsText(compiled.error()));
    return;
  }
  runtime::Isolate isolate;
  if (!isolate.RegisterNative(runtime::NativeFunction::Bind<GenericInput>("input"))) {
    state.SkipWithError("generic input registration failed");
    return;
  }
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
  const auto entry = dispatch.FindEntry(kGenericPath, "main");
  if (!entry) {
    state.SkipWithError("generic native entry is missing");
    return;
  }
  const auto expected = ExpectedGenericResult(state.range(0));
  for (auto iteration : state) {
    auto result = entry->Invoke({});
    if (!result || !result->IsInteger() || result->AsInteger() != expected) {
      state.SkipWithError("generic native checksum mismatch");
      break;
    }
    benchmark::DoNotOptimize(result);
  }
  const auto resources = (*compiler)->resource_counts();
  state.counters["aot_object_bytes"] = static_cast<double>(artifact->object.size());
  state.counters["executable_bytes"] = static_cast<double>(resources.executable_bytes);
  state.counters["result"] = static_cast<double>(expected);
  ReportGenericPeak(state);
}

struct VariantName final {
  GenericVariant variant;
  std::string_view name;
};

void RegisterGenericBenchmarks() {
  constexpr auto kVariants = std::to_array<VariantName>({
      {.variant = GenericVariant::Concrete, .name = "concrete"},
      {.variant = GenericVariant::Unbounded, .name = "unbounded"},
      {.variant = GenericVariant::Bounded, .name = "bounded"},
  });
  for (const auto& variant : kVariants) {
    for (const auto shape : {GenericShape::Scalar, GenericShape::Projection}) {
      const auto suffix = std::string(shape == GenericShape::Scalar ? "scalar/" : "projection/") +
                          std::string(variant.name);
      for (const auto phase : {GenericPhase::Definitions, GenericPhase::Applications}) {
        const auto name =
            "generics/check/" +
            std::string(phase == GenericPhase::Definitions ? "definitions/" : "applications/") +
            suffix;
        benchmark::RegisterBenchmark(name, GenericFrontend, variant.variant, shape, phase)
            ->Arg(kSmallGenericCount)
            ->Arg(kMediumGenericCount)
            ->Arg(kLargeGenericCount)
            ->UseRealTime();
      }
      benchmark::RegisterBenchmark("generics/vm/" + suffix, GenericVm, variant.variant, shape)
          ->Arg(kSmallGenericCount)
          ->Arg(kMediumGenericCount)
          ->Arg(kLargeGenericCount)
          ->UseRealTime();
      for (const auto tier : {NativeCodeTier::Baseline, NativeCodeTier::Optimized}) {
        const auto name =
            "generics/" +
            std::string(tier == NativeCodeTier::Baseline ? "baseline/" : "optimized/") + suffix;
        benchmark::RegisterBenchmark(name, GenericNative, variant.variant, shape, tier)
            ->Arg(kSmallGenericCount)
            ->Arg(kMediumGenericCount)
            ->Arg(kLargeGenericCount)
            ->UseRealTime();
      }
    }
  }
}

const auto kGenericRegistration = [] {
  RegisterGenericBenchmarks();
  return 0;
}();

}  // namespace
}  // namespace vela::benchmarks
