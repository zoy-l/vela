#include "compiler/frontend/parser.h"

#include <memory>
#include <utility>

#include "compiler/frontend/ast.h"
#include "compiler/frontend/token.h"

namespace vela::frontend {

std::unique_ptr<Expression> Parser::ParseArrayLiteral() {
  auto expression = MakeExpression(Expression::Kind::ArrayLiteral, Previous().location);
  while (!Check(TokenKind::RightBracket) && !IsAtEnd()) {
    auto element = ParseExpression();
    if (!element) {
      return nullptr;
    }
    expression->elements.push_back(std::move(element));
    if (!Match(TokenKind::Comma)) {
      break;
    }
  }
  if (!Expect(TokenKind::RightBracket, "expected ']' after array literal")) {
    return nullptr;
  }
  return expression;
}

std::unique_ptr<Expression> Parser::ParseMapLiteral() {
  AnnotatePrevious(TokenRole::MapDelimiter);
  auto expression = MakeExpression(Expression::Kind::MapLiteral, Previous().location);
  while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
    if (Match(TokenKind::Comma)) {
      AddDiagnostic(Previous().location, "expected map key before ','");
      continue;
    }
    MapEntry entry;
    if (Check(TokenKind::Colon)) {
      AddDiagnostic(Peek().location, "expected map key before ':'");
    } else {
      entry.key = ParseExpression();
    }
    if (!Expect(TokenKind::Colon, "expected ':' after map key")) {
      expression->entries.push_back(std::move(entry));
      break;
    }
    if (Check(TokenKind::Comma) || Check(TokenKind::RightBrace) || IsAtEnd()) {
      AddDiagnostic(Peek().location, "expected map value after ':'");
    } else {
      entry.value = ParseExpression();
    }
    // 错误输入仍保留已知的键和值供编辑器索引；编译入口以诊断拒绝执行。
    expression->entries.push_back(std::move(entry));
    if (!Match(TokenKind::Comma)) {
      break;
    }
  }
  if (Expect(TokenKind::RightBrace, "expected '}' after map literal")) {
    AnnotatePrevious(TokenRole::MapDelimiter);
  }
  return expression;
}

}  // namespace vela::frontend
