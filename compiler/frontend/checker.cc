#include "compiler/frontend/checker.h"

#include "compiler/frontend/lexer.h"
#include "compiler/frontend/parser.h"
#include "compiler/frontend/type_checker.h"

namespace vela::frontend {

auto CheckSource(std::string_view source) -> CheckResult {
  LexResult lexed = Lexer(source).Tokenize();
  if (!lexed.diagnostics.empty()) {
    return {.diagnostics = std::move(lexed.diagnostics), .function_count = 0};
  }

  ParseResult parsed = Parser(std::move(lexed.tokens)).Parse();
  if (!parsed.diagnostics.empty()) {
    return {.diagnostics = std::move(parsed.diagnostics), .function_count = 0};
  }

  TypeCheckResult checked = TypeChecker().Check(parsed.program);
  CheckResult result{
      .diagnostics = std::move(checked.diagnostics),
      .function_count = checked.function_count,
  };
  return result;
}

}  // namespace vela::frontend
