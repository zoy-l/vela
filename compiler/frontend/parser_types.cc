#include "compiler/frontend/parser.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "compiler/frontend/ast.h"
#include "compiler/frontend/token.h"

#include "compiler/frontend/type_limits.h"

namespace vela::frontend {

std::optional<Program::TypeAlias> Parser::ParseTypeAlias() {
  const auto name = Expect(TokenKind::Identifier, "expected type alias name");
  if (!name) {
    return std::nullopt;
  }
  auto type_parameters = ParseTypeParameters();
  if (!Expect(TokenKind::Equal, "expected '=' after type alias name")) {
    return std::nullopt;
  }
  const auto target = ParseType();
  if (!target || !Expect(TokenKind::Semicolon, "expected ';' after type alias")) {
    return std::nullopt;
  }
  return Program::TypeAlias{
      .name = name->lexeme,
      .target = *target,
      .location = name->location,
      .type_parameters = std::move(type_parameters),
  };
}

std::vector<std::string> Parser::ParseTypeParameters() {
  std::vector<std::string> parameters;
  if (!Match(TokenKind::Less)) {
    return parameters;
  }
  AnnotatePrevious(TokenRole::TypeDelimiter);
  while (true) {
    const auto name = Expect(TokenKind::Identifier, "expected generic parameter name");
    if (!name) {
      return {};
    }
    parameters.push_back(name->lexeme);
    if (!Match(TokenKind::Comma)) {
      break;
    }
  }
  if (Expect(TokenKind::Greater, "expected '>' after generic parameters")) {
    AnnotatePrevious(TokenRole::TypeDelimiter);
  }
  return parameters;
}

std::vector<TypeName> Parser::ParseTypeArguments() {
  std::vector<TypeName> arguments;
  std::size_t depth = 1;
  if (!Expect(TokenKind::Less, "expected '<' before type arguments")) {
    return arguments;
  }
  AnnotatePrevious(TokenRole::TypeDelimiter);
  while (true) {
    auto type = ParseType();
    if (!type) {
      return {};
    }
    arguments.push_back(std::move(*type));
    depth = std::max(depth, last_type_depth_ + 1);
    if (!Match(TokenKind::Comma)) {
      break;
    }
  }
  if (Expect(TokenKind::Greater, "expected '>' after type arguments")) {
    AnnotatePrevious(TokenRole::TypeDelimiter);
  }
  last_type_arguments_depth_ = depth;
  return arguments;
}

std::optional<TypeName> Parser::ParseType() {
  if (type_parse_depth_ >= kMaxTypeNesting) {
    AddDiagnostic(Peek().location, "type nesting exceeds the limit of 128");
    return std::nullopt;
  }
  ++type_parse_depth_;
  auto result = ParseTypeBody();
  --type_parse_depth_;
  return result;
}

std::optional<TypeName> Parser::ParseTypeBody() {
  TypeName result;
  std::size_t depth = 1;
  result.location = Peek().location;
  if (Match(TokenKind::LeftParen)) {
    auto parenthesized = ParseParenthesizedType();
    if (!parenthesized) {
      return std::nullopt;
    }
    result = std::move(*parenthesized);
    depth = last_type_depth_;
  } else if (Check(TokenKind::Identifier) || Check(TokenKind::Void)) {
    auto name = ParseQualifiedTypeName();
    if (!name) {
      return std::nullopt;
    }
    result.name = std::move(*name);
    if (Check(TokenKind::Less)) {
      result.kind = TypeName::Kind::Generic;
      result.arguments = ParseTypeArguments();
      depth = last_type_arguments_depth_;
    }
  } else {
    AddDiagnostic(Peek().location, "expected type name");
    return std::nullopt;
  }
  while (Check(TokenKind::Question) || Check(TokenKind::LeftBracket)) {
    if (depth >= kMaxTypeNesting) {
      AddDiagnostic(Peek().location, "type nesting exceeds the limit of 128");
      return std::nullopt;
    }
    ++depth;
    const auto kind = Match(TokenKind::Question) ? TypeName::Kind::Nullable : TypeName::Kind::Array;
    if (kind == TypeName::Kind::Array &&
        (!Expect(TokenKind::LeftBracket, "expected '[' in array type") ||
         !Expect(TokenKind::RightBracket, "expected ']' in array type"))) {
      return std::nullopt;
    }
    TypeName wrapper{
        .name = result.name + (kind == TypeName::Kind::Array ? "[]" : "?"),
        .location = result.location,
        .kind = kind,
        .arguments = {},
    };
    wrapper.arguments.push_back(std::move(result));
    result = std::move(wrapper);
  }
  if (depth > kMaxTypeNesting) {
    AddDiagnostic(result.location, "type nesting exceeds the limit of 128");
    return std::nullopt;
  }
  last_type_depth_ = depth;
  return result;
}

std::optional<std::string> Parser::ParseQualifiedTypeName() {
  auto name = tokens_.subspan(current_++).front().lexeme;
  while (Match(TokenKind::Dot)) {
    const auto member = Expect(TokenKind::Identifier, "expected qualified type name after '.'");
    if (!member) {
      return std::nullopt;
    }
    name += '.' + member->lexeme;
  }
  return name;
}

std::optional<TypeName> Parser::ParseParenthesizedType() {
  TypeName result;
  result.location = Previous().location;
  std::size_t depth = 1;
  result.kind = TypeName::Kind::Function;
  result.name = "(";
  while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
    auto parameter = ParseType();
    if (!parameter) {
      return std::nullopt;
    }
    if (!result.arguments.empty()) {
      result.name += ", ";
    }
    result.name += parameter->name;
    result.arguments.push_back(std::move(*parameter));
    depth = std::max(depth, last_type_depth_ + 1);
    if (!Match(TokenKind::Comma)) {
      break;
    }
  }
  if (!Expect(TokenKind::RightParen, "expected ')' after parameter types")) {
    return std::nullopt;
  }
  if (Match(TokenKind::Arrow)) {
    auto returned = ParseType();
    if (!returned) {
      return std::nullopt;
    }
    result.name += ") => " + returned->name;
    result.arguments.push_back(std::move(*returned));
    depth = std::max(depth, last_type_depth_ + 1);
  } else if (result.arguments.size() == 1) {
    auto grouped = std::move(result.arguments.front());
    result = std::move(grouped);
    --depth;
  } else {
    AddDiagnostic(Peek().location, "expected '=>' after function parameter types");
    return std::nullopt;
  }
  last_type_depth_ = depth;
  return result;
}

bool Parser::IsTypeApplication() const {
  if (!Check(TokenKind::Less)) {
    return false;
  }
  std::size_t depth = 0;
  for (std::size_t index = current_; index < tokens_.size(); ++index) {
    const auto kind = tokens_.subspan(index).front().kind;
    if (kind == TokenKind::Less) {
      ++depth;
    } else if (kind == TokenKind::Greater && --depth == 0) {
      const auto next = tokens_.subspan(index + 1).front().kind;
      return next == TokenKind::LeftParen || next == TokenKind::Dot || next == TokenKind::Is ||
             next == TokenKind::As || next == TokenKind::Semicolon || next == TokenKind::Comma ||
             next == TokenKind::RightParen || next == TokenKind::RightBracket;
    } else if (kind == TokenKind::Semicolon || kind == TokenKind::LeftBrace ||
               kind == TokenKind::RightBrace || kind == TokenKind::EndOfFile) {
      return false;
    }
  }
  return false;
}

}  // namespace vela::frontend
