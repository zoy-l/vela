#include "compiler/frontend/checker.h"

#include <span>
#include <string_view>
#include <utility>

#include "compiler/frontend/ast.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/parser.h"
#include "compiler/frontend/source_loader.h"
#include "compiler/frontend/type_checker.h"

namespace vela::frontend {
CheckResult CheckParsed(ParseResult parsed, DiagnosticOptions options) {
  if (!parsed.diagnostics.empty()) {
    return {.diagnostics = std::move(parsed.diagnostics), .function_count = 0};
  }
  return CheckProgram(parsed.program, options);
}

CheckResult CheckProgram(Program& program, DiagnosticOptions options) {
  auto checked = TypeChecker(nullptr, options).Check(program);
  return {.diagnostics = std::move(checked.diagnostics), .function_count = checked.function_count};
}

CheckResult CheckFile(std::string_view path,
                      std::span<const SourceInput> overlays,
                      DiagnosticOptions options) {
  return CheckParsed(LoadProgram(path, overlays), options);
}

}  // namespace vela::frontend
