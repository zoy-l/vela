#include "compiler/tooling/semantic_index.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/builtin.h"
#include "compiler/frontend/ast.h"
#include "compiler/frontend/builtins.h"
#include "compiler/frontend/lexer.h"
#include "compiler/frontend/parser.h"
#include "compiler/frontend/token.h"
#include "compiler/frontend/type_checker.h"

namespace vela::tooling {
namespace {

using frontend::Token;
using frontend::TokenKind;
using frontend::TokenRole;

std::optional<frontend::FunctionDescriptor> ArrayCallParameters(
    std::string_view name,
    const frontend::TypeDescriptor& receiver,
    const frontend::TypeDescriptor& element) {
  if (name == "join") {
    return frontend::BuiltinSignature(Builtin::ArrayJoin);
  }
  if (const auto algorithm = frontend::FindArrayAlgorithm(name)) {
    auto accumulator = element;
    if (*algorithm == frontend::ArrayAlgorithm::Map ||
        *algorithm == frontend::ArrayAlgorithm::Reduce) {
      accumulator.name = "R";
    }
    return frontend::ArrayAlgorithmSignature(*algorithm, element, accumulator);
  }
  for (const auto& [method, builtin] : frontend::ArrayMethods()) {
    if (method == name) {
      return frontend::ArrayMethodSignature(builtin, receiver, element);
    }
  }
  return std::nullopt;
}

std::optional<TokenKind> ArgumentClosing(const Token& token) {
  switch (token.kind) {
    case TokenKind::LeftParen:
      return TokenKind::RightParen;
    case TokenKind::LeftBracket:
      return TokenKind::RightBracket;
    case TokenKind::LeftBrace:
      return TokenKind::RightBrace;
    case TokenKind::Less:
      return token.role == TokenRole::TypeDelimiter ? std::optional(TokenKind::Greater)
                                                    : std::nullopt;
    case TokenKind::StringHead:
      return TokenKind::StringTail;
    default:
      return std::nullopt;
  }
}

std::optional<std::size_t> CalleeToken(const std::vector<Token>& tokens, std::size_t opening) {
  if (opening == 0) {
    return std::nullopt;
  }
  auto index = opening - 1;
  const auto& previous = tokens.at(index);
  if (previous.kind == TokenKind::Greater && previous.role == TokenRole::TypeDelimiter) {
    std::size_t depth = 1;
    while (index > 0 && depth != 0) {
      const auto& token = tokens.at(--index);
      if (token.role == TokenRole::TypeDelimiter) {
        if (token.kind == TokenKind::Greater) {
          ++depth;
        } else if (token.kind == TokenKind::Less) {
          --depth;
        }
      }
    }
    if (depth != 0 || index == 0) {
      return std::nullopt;
    }
    --index;
  }
  const auto kind = tokens.at(index).kind;
  return kind == TokenKind::Identifier || kind == TokenKind::Print ? std::optional(index)
                                                                   : std::nullopt;
}

const Token& ArgumentEnd(std::span<const Token> arguments) {
  std::vector<TokenKind> closing;
  for (const auto& token : arguments) {
    if (closing.empty() &&
        (token.kind == TokenKind::RightParen || token.kind == TokenKind::RightBrace ||
         token.kind == TokenKind::Semicolon)) {
      return token;
    }
    if (const auto close = ArgumentClosing(token)) {
      closing.push_back(*close);
    } else if (!closing.empty() && token.kind == closing.back()) {
      closing.pop_back();
    }
  }
  return arguments.back();
}

std::string ArgumentSource(std::string_view source,
                           std::span<const Token> arguments,
                           const Token& opening,
                           std::size_t offset) {
  const auto& end = ArgumentEnd(arguments);
  const auto suffix = end.offset + (end.kind == TokenKind::RightParen ? end.length : 0);
  const auto begin = opening.offset + opening.length;
  std::string text(source.substr(0, begin));
  for (const auto character : source.substr(begin, offset - begin)) {
    text += character == '\n' ? '\n' : ' ';
  }
  text += "__vela_completion__)";
  if (end.kind == TokenKind::RightBrace) {
    text += ';';
  }
  text += source.substr(suffix);
  // 实参尚未输入时也构造真正的 Call AST；实参错误不应阻止展示声明契约。
  std::vector<TokenKind> closing;
  for (const auto& token : frontend::Lexer(text).Tokenize().tokens) {
    if (const auto close = ArgumentClosing(token)) {
      closing.push_back(*close);
    } else if (!closing.empty() && token.kind == closing.back()) {
      closing.pop_back();
    }
  }
  for (auto iterator = closing.rbegin(); iterator != closing.rend(); ++iterator) {
    if (*iterator == TokenKind::RightBrace) {
      text += ";}";
    } else if (*iterator == TokenKind::RightParen) {
      text += ')';
    } else if (*iterator == TokenKind::RightBracket) {
      text += ']';
    }
  }
  return text;
}

bool IsStringText(const Token& token, std::size_t offset) {
  if (offset >= token.offset + token.length) {
    return false;
  }
  switch (token.kind) {
    case TokenKind::StringLiteral:
    case TokenKind::StringHead:
    case TokenKind::StringMiddle:
    case TokenKind::StringTail:
      return true;
    default:
      return false;
  }
}

std::optional<std::vector<std::size_t>> ArgumentOpenings(const std::vector<Token>& tokens,
                                                         std::size_t offset) {
  std::vector<std::size_t> opening;
  for (std::size_t index = 0; index < tokens.size(); ++index) {
    const auto& token = tokens.at(index);
    if (token.offset >= offset) {
      break;
    }
    if (IsStringText(token, offset)) {
      return std::nullopt;
    }
    if (ArgumentClosing(token)) {
      opening.push_back(index);
    } else if (!opening.empty() && ArgumentClosing(tokens.at(opening.back())) == token.kind) {
      opening.pop_back();
    }
  }
  return opening;
}

std::string ArgumentName(std::span<const Token> argument) {
  if (argument.size() > 1 && argument.front().kind == TokenKind::Identifier &&
      argument.subspan(1).front().kind == TokenKind::Colon) {
    return argument.front().lexeme;
  }
  return {};
}

bool IsArgumentNameSlot(std::span<const Token> argument, std::size_t offset) {
  if (argument.empty() || argument.front().offset >= offset) {
    return true;
  }
  const auto& token = argument.front();
  return token.kind == TokenKind::Identifier && offset <= token.offset + token.length;
}

void SetArgumentPrefix(ArgumentCompletionQuery& query,
                       std::span<const Token> argument,
                       std::size_t offset) {
  if (argument.empty()) {
    return;
  }
  const auto& first = argument.front();
  query.prefix = first.offset < offset ? first.lexeme.substr(0, offset - first.offset) : "";
  query.has_colon = !ArgumentName(argument).empty();
}

std::optional<ArgumentCompletionQuery> ReadArgumentSlots(std::span<const Token> arguments,
                                                         std::size_t offset) {
  ArgumentCompletionQuery query{
      .source = {},
      .prefix = {},
      .supplied_names = {},
  };
  std::vector<TokenKind> closing;
  std::size_t argument_start = 0;
  bool selected = false;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const auto& token = arguments.subspan(index).front();
    const auto boundary = token.kind == TokenKind::Comma || token.kind == TokenKind::RightParen ||
                          token.kind == TokenKind::RightBrace ||
                          token.kind == TokenKind::Semicolon || token.kind == TokenKind::EndOfFile;
    if (closing.empty() && boundary) {
      const auto argument = arguments.subspan(argument_start, index - argument_start);
      auto name = ArgumentName(argument);
      if (!selected && token.offset >= offset) {
        if (!IsArgumentNameSlot(argument, offset)) {
          return std::nullopt;
        }
        SetArgumentPrefix(query, argument, offset);
        selected = true;
      } else if (!name.empty()) {
        query.supplied_names.insert(std::move(name));
      } else if (!selected && !argument.empty()) {
        ++query.positional_arguments;
      }
      if (token.kind != TokenKind::Comma) {
        break;
      }
      argument_start = index + 1;
    } else if (const auto close = ArgumentClosing(token)) {
      closing.push_back(*close);
    } else if (!closing.empty() && token.kind == closing.back()) {
      closing.pop_back();
    }
  }
  return selected ? std::optional(std::move(query)) : std::nullopt;
}

}  // namespace

std::optional<ArgumentCompletionQuery> PrepareArgumentCompletion(std::string_view source,
                                                                 std::size_t offset) {
  auto lexed = frontend::Lexer(source, 0, frontend::CommentRetention::Preserve).Tokenize();
  for (const auto& comment : lexed.comments) {
    if (offset >= comment.offset && offset <= comment.offset + comment.length) {
      return std::nullopt;
    }
  }
  // 复用 parser 的泛型标记，比较运算中的逗号不能误作类型实参。
  static_cast<void>(frontend::Parser(lexed.tokens).Parse());
  const auto& tokens = lexed.tokens;
  const auto opening = ArgumentOpenings(tokens, offset);
  if (!opening || opening->empty()) {
    return std::nullopt;
  }
  const auto start = opening->back();
  const auto& open = tokens.at(start);
  if (open.kind != TokenKind::LeftParen) {
    return std::nullopt;
  }
  const auto callee = CalleeToken(tokens, start);
  if (!callee) {
    return std::nullopt;
  }
  auto query = ReadArgumentSlots(std::span(tokens).subspan(start + 1), offset);
  if (query) {
    query->source = ArgumentSource(source, std::span(tokens).subspan(start + 1), open, offset);
    query->callee_offset = tokens.at(*callee).offset;
  }
  return query;
}

std::optional<frontend::FunctionDescriptor> SemanticIndex::FindCallParameters(
    std::size_t callee_offset) const {
  const auto found = calls_.find(Key{0, callee_offset});
  if (found == calls_.end()) {
    return std::nullopt;
  }
  const auto& call = *found->second;
  if (const auto builtin = model_.builtin_calls.find(&call);
      builtin != model_.builtin_calls.end() &&
      (builtin->second == Builtin::NewByteArray || builtin->second == Builtin::NewRegExp)) {
    return frontend::BuiltinSignature(builtin->second);
  }
  if (const auto async = model_.async_calls.find(&call); async != model_.async_calls.end()) {
    return async->second.signature;
  }
  const auto& callee = *call.callee;
  // 函数值只接受位置实参；不把内部的数字参数名暴露为命名实参补全。
  if (model_.bindings.contains(&callee) || model_.globals.contains(callee.text) ||
      model_.expression_types.contains(&callee)) {
    return std::nullopt;
  }
  if (callee.kind == frontend::Expression::Kind::Name) {
    const auto type = model_.types.find(callee.text);
    const auto owner =
        model_.classes.find(type == model_.types.end() ? callee.text : type->second.name);
    if (owner != model_.classes.end()) {
      return owner->second.constructor;
    }
    if (const auto function = model_.functions.find(callee.text);
        function != model_.functions.end()) {
      return function->second;
    }
    if (const auto builtin =
            frontend::FindBuiltin(frontend::TypeDescriptor::Kind::Invalid, callee.text)) {
      return frontend::BuiltinSignature(*builtin);
    }
  } else if (callee.kind == frontend::Expression::Kind::Member) {
    return FindMemberCallParameters(call);
  }
  return std::nullopt;
}

std::optional<frontend::FunctionDescriptor> SemanticIndex::FindMemberCallParameters(
    const frontend::Expression& call) const {
  const auto& callee = *call.callee;
  auto receiver = ReceiverFor(*callee.object);
  if (const auto member = model_.static_members.find(&call);
      member != model_.static_members.end()) {
    receiver.type.name = member->second.owner;
  }
  if (const auto owner = model_.classes.find(receiver.type.name); owner != model_.classes.end()) {
    const auto method = owner->second.methods.find(callee.text);
    if (method != owner->second.methods.end()) {
      return method->second;
    }
  }
  if (const auto owner = model_.interfaces.find(receiver.type.name);
      owner != model_.interfaces.end()) {
    const auto method = owner->second.methods.find(callee.text);
    if (method != owner->second.methods.end()) {
      return method->second;
    }
  }
  if (const auto builtin = frontend::FindBuiltin(receiver.type.kind, callee.text)) {
    return frontend::BuiltinSignature(*builtin);
  }
  if (receiver.type.kind == frontend::TypeDescriptor::Kind::Array) {
    const auto element = model_.array_elements.find(receiver.type.name);
    if (element != model_.array_elements.end()) {
      return ArrayCallParameters(callee.text, receiver.type, element->second);
    }
  }
  return std::nullopt;
}

}  // namespace vela::tooling
