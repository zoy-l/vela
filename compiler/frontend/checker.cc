#include "compiler/frontend/checker.h"

#include "compiler/frontend/lexer.h"
#include "compiler/frontend/parser.h"
#include "compiler/frontend/type_checker.h"

namespace vela::frontend {

CheckResult CheckSource(std::string_view source) {
  LexResult lexed = Lexer(source).Tokenize();
  if (!lexed.diagnostics.empty()) {
    return {std::move(lexed.diagnostics), 0};
  }

  ParseResult parsed = Parser(std::move(lexed.tokens)).Parse();
  if (!parsed.diagnostics.empty()) {
    return {std::move(parsed.diagnostics), 0};
  }

  TypeCheckResult checked = TypeChecker().Check(parsed.program);
  return {std::move(checked.diagnostics), checked.function_count};
}

}  // namespace vela::frontend
