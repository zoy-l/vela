#include "compiler/tooling/semantic/semantic_index.h"

#include <cstddef>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/node_hash_map.h>

#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/syntax/parser.h"
#include "compiler/frontend/syntax/token.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/tooling/semantic/semantic_syntax.h"

namespace vela::tooling {
namespace {

using frontend::Token;
using frontend::TokenKind;
using frontend::TokenRole;

std::optional<TokenKind> ArgumentClosing(const Token& token) {
  switch (token.kind) {
    case TokenKind::Less:
      return token.role == TokenRole::TypeDelimiter ? std::optional(TokenKind::Greater)
                                                    : std::nullopt;
    case TokenKind::StringHead:
      return TokenKind::StringTail;
    default:
      return ClosingDelimiter(token.kind);
  }
}

std::optional<std::size_t> CalleeToken(const std::vector<Token>& tokens, std::size_t opening) {
  if (opening == 0) {
    return std::nullopt;
  }
  auto index = opening - 1;
  const auto& previous = tokens[index];
  if (previous.kind == TokenKind::Greater && previous.role == TokenRole::TypeDelimiter) {
    std::size_t depth = 1;
    while (index > 0 && depth != 0) {
      const auto& token = tokens[--index];
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
  const auto kind = tokens[index].kind;
  return frontend::IsMemberName(kind) || kind == TokenKind::Print ? std::optional(index)
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
    TrackDelimiter(closing, token.kind, ArgumentClosing(token));
  }
  return arguments.back();
}

std::string ArgumentSource(const frontend::SourceSnapshot& snapshot,
                           std::span<const Token> arguments,
                           const Token& opening,
                           std::size_t offset) {
  const auto source = std::string_view(snapshot.text);
  const auto& tokens = snapshot.tokens;
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
  for (const auto& token : tokens) {
    if (token.offset < begin) {
      TrackDelimiter(closing, token.kind, ArgumentClosing(token));
    }
  }
  TrackDelimiter(closing, TokenKind::RightParen, std::nullopt);
  for (const auto& token : tokens) {
    if (token.offset >= suffix) {
      TrackDelimiter(closing, token.kind, ArgumentClosing(token));
    }
  }
  for (const auto kind : std::views::reverse(closing)) {
    if (kind == TokenKind::RightBrace) {
      text += ";}";
    } else if (kind == TokenKind::RightParen) {
      text += ')';
    } else if (kind == TokenKind::RightBracket) {
      text += ']';
    }
  }
  return text;
}

bool IsStringText(const Token& token, std::size_t offset) {
  return offset < token.offset + token.length && frontend::IsStringToken(token.kind);
}

std::optional<std::vector<std::size_t>> ArgumentOpenings(const std::vector<Token>& tokens,
                                                         std::size_t offset) {
  std::vector<std::size_t> opening;
  for (std::size_t index = 0; index < tokens.size(); ++index) {
    const auto& token = tokens[index];
    if (token.offset >= offset) {
      break;
    }
    if (IsStringText(token, offset)) {
      return std::nullopt;
    }
    if (ArgumentClosing(token)) {
      opening.push_back(index);
    } else if (!opening.empty() && ArgumentClosing(tokens[opening.back()]) == token.kind) {
      opening.pop_back();
    }
  }
  return opening;
}

std::string ArgumentName(std::span<const Token> argument) {
  if (argument.size() > 1 && argument.front().kind == TokenKind::Identifier &&
      argument[1].kind == TokenKind::Colon) {
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
    const auto& token = arguments[index];
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
    } else {
      TrackDelimiter(closing, token.kind, ArgumentClosing(token));
    }
  }
  return selected ? std::optional(std::move(query)) : std::nullopt;
}

}  // namespace

std::optional<ArgumentCompletionQuery> PrepareArgumentCompletion(
    const frontend::SourceSnapshot& snapshot,
    std::size_t offset) {
  if (IsInComment(snapshot.comments, offset)) {
    return std::nullopt;
  }
  // 快照已保留 parser 的泛型标记，比较运算中的逗号不能误作类型实参。
  const auto& tokens = snapshot.tokens;
  const auto opening = ArgumentOpenings(tokens, offset);
  if (!opening || opening->empty()) {
    return std::nullopt;
  }
  const auto start = opening->back();
  const auto& open = tokens[start];
  if (open.kind != TokenKind::LeftParen) {
    return std::nullopt;
  }
  const auto callee = CalleeToken(tokens, start);
  if (!callee) {
    return std::nullopt;
  }
  auto query = ReadArgumentSlots(std::span(tokens).subspan(start + 1), offset);
  if (query) {
    query->source = ArgumentSource(snapshot, std::span(tokens).subspan(start + 1), open, offset);
    query->callee_offset = tokens[*callee].offset;
  }
  return query;
}

const frontend::FunctionDescriptor* SemanticIndex::FindCallParameters(
    std::size_t callee_offset) const {
  const auto found = calls_.find(Key{0, callee_offset});
  if (found == calls_.end()) {
    return nullptr;
  }
  const auto& call = *found->second;
  const auto& facts = FactsFor(call);
  const auto target = facts.call_targets.find(&call);
  if (target == facts.call_targets.end() ||
      target->second.argument_names == frontend::CallArgumentNames::Positional) {
    return nullptr;
  }
  return &target->second.signature;
}

const frontend::FunctionDescriptor* SemanticIndex::FindMemberValueParameters(
    const frontend::Expression& expression) const {
  const auto& facts = FactsFor(expression);
  for (const auto* members : {
           &facts.static_members,
           &facts.instance_members,
           &facts.interface_members,
           &facts.super_members,
       }) {
    const auto member = members->find(&expression);
    if (member != members->end()) {
      const auto& reference = member->second;
      if (const auto owner = model_.classes.find(reference.owner); owner != model_.classes.end()) {
        return &owner->second.methods.at(reference.member);
      }
      return &model_.interfaces.at(reference.owner).methods.at(reference.member);
    }
  }
  return nullptr;
}

}  // namespace vela::tooling
