#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

#include "compiler/frontend/modules/source_loader.h"
#include "compiler/frontend/syntax/parser.h"
#include "compiler/tooling/semantic/semantic_index.h"

namespace vela::benchmarks {
namespace {

enum class CompletionReuse : std::uint8_t { Fresh, ParsedModules, Index };

void CompleteProject(benchmark::State& state, CompletionReuse reuse) {
  std::vector<std::string> paths;
  std::vector<std::string> texts;
  const auto modules_count = static_cast<std::size_t>(state.range(0));
  paths.reserve(modules_count + 1);
  texts.reserve(modules_count + 1);
  paths.emplace_back("completion-main.vela");
  texts.emplace_back();
  for (std::size_t index = 0; index < modules_count; ++index) {
    paths.push_back("completion-module" + std::to_string(index) + ".vela");
    texts.front() += "import '" + paths.back() + "';";
    std::string module;
    constexpr int kDeclarationsPerModule = 8;
    for (int declaration = 0; declaration < kDeclarationsPerModule; ++declaration) {
      module += "export class Record" + std::to_string(index) + "_" + std::to_string(declaration) +
                " { value: int = 42; read(): int { "
                "return this.value; } }";
    }
    texts.push_back(std::move(module));
  }
  texts.front() += "func main(): void { const record = Record0_0(); print(record.value); }";
  const auto offset = texts.front().rfind(".value") + 1;
  std::vector<frontend::SourceInput> inputs;
  inputs.reserve(paths.size());
  for (std::size_t index = 0; index < paths.size(); ++index) {
    inputs.push_back({.path = paths.at(index), .text = texts.at(index)});
  }
  frontend::ModuleCache modules;
  const tooling::SemanticIndex retained(paths.front(), modules, inputs);
  for (auto iteration : state) {
    std::optional<tooling::SemanticIndex> current;
    const tooling::SemanticIndex* index = &retained;
    if (reuse == CompletionReuse::Fresh) {
      current.emplace(paths.front(), inputs);
      index = &*current;
    } else if (reuse == CompletionReuse::ParsedModules) {
      current.emplace(paths.front(), modules, inputs);
      index = &*current;
    }
    auto completions = index->Complete(offset);
    if (std::ranges::none_of(completions, [](const auto& item) { return item.label == "value"; })) {
      state.SkipWithError("Project member completion failed");
      break;
    }
    benchmark::DoNotOptimize(completions);
  }
}

BENCHMARK_CAPTURE(CompleteProject, Fresh, CompletionReuse::Fresh)
    ->Name("tooling/completion/fresh")
    ->Arg(8)
    ->Arg(32);
BENCHMARK_CAPTURE(CompleteProject, ParsedModules, CompletionReuse::ParsedModules)
    ->Name("tooling/completion/shared_parse")
    ->Arg(8)
    ->Arg(32);
BENCHMARK_CAPTURE(CompleteProject, Index, CompletionReuse::Index)
    ->Name("tooling/completion/reuse_index")
    ->Arg(8)
    ->Arg(32);

void CompleteInheritedConstructor(benchmark::State& state) {
  std::string source;
  const auto count = state.range(0);
  constexpr int kFieldsPerClass = 4;
  for (std::int64_t owner = 0; owner < count; ++owner) {
    source += "class Record" + std::to_string(owner);
    if (owner != 0) {
      source += " extends Record" + std::to_string(owner - 1);
    }
    source += " {";
    for (int field = 0; field < kFieldsPerClass; ++field) {
      source += " field" + std::to_string(owner) + "_" + std::to_string(field) + ": int = 42;";
    }
    source += " }";
  }
  source += "func main(): void { const value = Record" + std::to_string(count - 1) + "(); }";
  const auto parsed = frontend::ParseSourceSnapshot(source);
  const auto query = tooling::PrepareArgumentCompletion(*parsed.source, source.rfind("();") + 1);
  if (!query) {
    state.SkipWithError("missing constructor argument query");
    return;
  }
  const frontend::SourceInput input{.path = "constructor-completion.vela", .text = query->source};
  frontend::ModuleCache modules;
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    const tooling::SemanticIndex index(input.path, modules, std::span(&input, 1));
    auto completions = index.CompleteArguments(*query);
    if (std::ranges::none_of(completions,
                             [](const auto& item) { return item.label == "field0_0:"; })) {
      state.SkipWithError("inherited constructor completion failed");
      return;
    }
    benchmark::DoNotOptimize(completions);
  }
}

BENCHMARK(CompleteInheritedConstructor)
    ->Name("tooling/completion/inherited_constructor")
    ->Arg(8)
    ->Arg(32)
    ->UseRealTime();

}  // namespace
}  // namespace vela::benchmarks
