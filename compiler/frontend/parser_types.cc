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
  if (!type_parameters || !Expect(TokenKind::Equal, "expected '=' after type alias name")) {
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
      .type_parameters = std::move(*type_parameters),
  };
}

std::optional<std::vector<TypeParameter>> Parser::ParseTypeParameters() {
  std::vector<TypeParameter> parameters;
  if (!Match(TokenKind::Less)) {
    return parameters;
  }
  AnnotatePrevious(TokenRole::TypeDelimiter);
  while (true) {
    const auto name = Expect(TokenKind::Identifier, "expected generic parameter name");
    if (!name) {
      return std::nullopt;
    }
    TypeParameter parameter{
        .name = name->lexeme,
        .location = name->location,
        .bound = std::nullopt,
        .identity = {},
    };
    if (Match(TokenKind::Extends)) {
      auto bound = ParseType();
      if (!bound) {
        return std::nullopt;
      }
      parameter.bound = std::move(*bound);
    }
    parameters.push_back(std::move(parameter));
    if (!Match(TokenKind::Comma)) {
      break;
    }
  }
  if (!MatchTypeGreater()) {
    AddDiagnostic(Peek().location, "expected '>' after generic parameters");
    return std::nullopt;
  }
  AnnotatePrevious(TokenRole::TypeDelimiter);
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
  if (MatchTypeGreater()) {
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
  return ParseTypeSuffixes(std::move(result), depth);
}

std::optional<TypeName> Parser::ParseTypeSuffixes(TypeName result, std::size_t depth) {
  while (Check(TokenKind::Question) || Check(TokenKind::QuestionQuestion) ||
         Check(TokenKind::LeftBracket)) {
    if (depth >= kMaxTypeNesting) {
      AddDiagnostic(Peek().location, "type nesting exceeds the limit of 128");
      return std::nullopt;
    }
    ++depth;
    if (Match(TokenKind::QuestionQuestion)) {
      for (int repeat = 0; repeat < 2; ++repeat) {
        ++depth;
        TypeName wrapper{
            .name = result.name + "?",
            .location = result.location,
            .kind = TypeName::Kind::Nullable,
            .arguments = {},
            .parameter = {},
        };
        wrapper.arguments.push_back(std::move(result));
        result = std::move(wrapper);
      }
      continue;
    }
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
        .parameter = {},
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
  auto name = Advance().lexeme;
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

void Parser::IndexTypeApplications() {
  type_application_ends_.assign(tokens_.size(), tokens_.size());
  std::vector<std::size_t> opening;
  for (std::size_t index = 0; index < tokens_.size(); ++index) {
    const auto kind = tokens_.subspan(index).front().kind;
    if (kind == TokenKind::Less) {
      opening.push_back(index);
      continue;
    }
    std::size_t count = 0;
    if (kind == TokenKind::Greater) {
      count = 1;
    } else if (kind == TokenKind::ShiftRight) {
      count = 2;
    } else if (kind == TokenKind::ShiftUnsignedRight) {
      count = 3;
    } else if (kind == TokenKind::Semicolon || kind == TokenKind::LeftBrace ||
               kind == TokenKind::RightBrace || kind == TokenKind::EndOfFile) {
      opening.clear();
    }
    while (count != 0 && !opening.empty()) {
      type_application_ends_.at(opening.back()) = index;
      opening.pop_back();
      --count;
    }
  }
}

bool Parser::IsTypeApplication() {
  if (!Check(TokenKind::Less)) {
    return false;
  }
  if (type_application_ends_.empty()) {
    // 每个 '<' 共享一次线性配对，避免比较表达式反复扫描同一后缀。
    IndexTypeApplications();
  }
  const auto end = type_application_ends_.at(current_);
  if (end == tokens_.size()) {
    return false;
  }
  const auto next = tokens_.subspan(end + 1).front().kind;
  return next == TokenKind::LeftParen || next == TokenKind::Dot || next == TokenKind::Is ||
         next == TokenKind::As || next == TokenKind::Semicolon || next == TokenKind::Comma ||
         next == TokenKind::RightParen || next == TokenKind::RightBracket;
}

bool Parser::MatchTypeGreater() {
  const auto kind = Peek().kind;
  if (kind == TokenKind::Greater) {
    auto& token = tokens_.subspan(current_).front();
    token.type_closing_length = Peek().offset - token.offset + 1;
    Advance();
    return true;
  }
  TokenKind suffix = kind;
  switch (kind) {
    case TokenKind::GreaterEqual:
      suffix = TokenKind::Equal;
      break;
    case TokenKind::ShiftRight:
      suffix = TokenKind::Greater;
      break;
    case TokenKind::ShiftRightEqual:
      suffix = TokenKind::GreaterEqual;
      break;
    case TokenKind::ShiftUnsignedRight:
      suffix = TokenKind::ShiftRight;
      break;
    case TokenKind::ShiftUnsignedRightEqual:
      suffix = TokenKind::ShiftRightEqual;
      break;
    default:
      return false;
  }
  auto remainder = Peek();
  auto& token = tokens_.subspan(current_).front();
  token.type_closing_length = remainder.offset - token.offset + 1;
  previous_split_ = remainder;
  previous_split_->kind = TokenKind::Greater;
  previous_split_->lexeme = ">";
  previous_split_->length = 1;
  previous_index_ = current_;
  remainder.kind = suffix;
  remainder.lexeme.erase(0, 1);
  ++remainder.offset;
  ++remainder.location.column;
  --remainder.length;
  remainder_ = std::move(remainder);
  return true;
}

}  // namespace vela::frontend
