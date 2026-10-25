#include "compiler/frontend/checker.h"

#include <span>
#include <string_view>
#include <utility>

#include "compiler/frontend/ast.h"
#include "compiler/frontend/parser.h"
#include "compiler/frontend/source_loader.h"
#include "compiler/frontend/specialization.h"
#include "compiler/frontend/type_checker.h"

namespace vela::frontend {
namespace {

auto CheckParsed(ParseResult parsed) -> CheckResult {
  if (!parsed.diagnostics.empty()) {
    return {.diagnostics = std::move(parsed.diagnostics), .function_count = 0};
  }
  return CheckProgram(parsed.program);
}

}  // namespace

auto CheckProgram(Program& program) -> CheckResult {
  auto specialized = SpecializeGenerics(program);
  if (!specialized.empty()) {
    return {.diagnostics = std::move(specialized), .function_count = 0};
  }
  auto checked = TypeChecker().Check(program);
  return {.diagnostics = std::move(checked.diagnostics), .function_count = checked.function_count};
}

auto CheckSource(std::string_view source) -> CheckResult {
  const SourceInput input{.path = "main.vela", .text = source};
  return CheckParsed(LoadSources(std::span(&input, 1), input.path));
}
auto CheckFile(std::string_view path, std::span<const SourceInput> overlays) -> CheckResult {
  return CheckParsed(LoadProgram(path, overlays));
}

auto CheckModules(std::span<const SourceInput> sources, std::string_view entry) -> CheckResult {
  return CheckParsed(LoadSources(sources, entry));
}

}  // namespace vela::frontend
