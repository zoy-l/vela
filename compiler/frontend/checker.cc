#include "compiler/frontend/checker.h"

#include <string_view>
#include <utility>

#include "compiler/frontend/parser.h"
#include "compiler/frontend/source_loader.h"
#include "compiler/frontend/type_checker.h"

namespace vela::frontend {
namespace {

auto CheckParsed(ParseResult parsed) -> CheckResult {
  if (!parsed.diagnostics.empty()) {
    return {.diagnostics = std::move(parsed.diagnostics), .function_count = 0};
  }
  auto checked = TypeChecker().Check(parsed.program);
  return {.diagnostics = std::move(checked.diagnostics), .function_count = checked.function_count};
}

}  // namespace

auto CheckSource(std::string_view source) -> CheckResult {
  return CheckParsed(ParseSource(source));
}
auto CheckFile(std::string_view path) -> CheckResult {
  return CheckParsed(LoadProgram(path));
}

}  // namespace vela::frontend
