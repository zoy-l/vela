#include "compiler/tooling/semantic_index.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "base/builtin.h"
#include "compiler/frontend/ast.h"
#include "compiler/frontend/builtins.h"
#include "compiler/frontend/token.h"
#include "compiler/frontend/type_checker.h"

namespace vela::tooling {
namespace {

using Kind = frontend::TypeDescriptor::Kind;

std::string BuiltinDetail(const frontend::FunctionDescriptor& signature) {
  std::string text = "(";
  for (const auto& parameter : signature.parameters) {
    if (text.back() != '(') {
      text += ", ";
    }
    text += parameter.name + ": " + parameter.type.name;
  }
  return text + "): " + signature.return_type.name;
}

void AddBuiltins(std::vector<Completion>& result, Kind kind) {
  if (kind == Kind::Invalid) {
    result.push_back(
        {.label = "print", .detail = "(value): void", .kind = CompletionKind::Function});
  }
  constexpr auto kNames = std::to_array<std::string_view>({
      "StringBuilder",
      "toString",
      "toInt",
      "toDouble",
      "append",
      "slice",
      "indexOf",
      "includes",
      "startsWith",
      "endsWith",
      "codePoints",
      "graphemes",
      "graphemeSlice",
  });
  for (const auto name : kNames) {
    if (const auto builtin = frontend::FindBuiltin(kind, name)) {
      result.push_back({
          .label = std::string(name),
          .detail = BuiltinDetail(frontend::BuiltinSignature(*builtin)),
          .kind = kind == Kind::Invalid ? CompletionKind::Function : CompletionKind::Method,
      });
    }
  }
}

void AddArrayElementMethods(std::vector<Completion>& result,
                            const frontend::TypeDescriptor& element) {
  result.push_back({
      .label = "fill",
      .detail = "(value: " + element.name + "): void",
      .kind = CompletionKind::Method,
  });
  if (frontend::CanFormatString(element)) {
    result.push_back({
        .label = "join",
        .detail = BuiltinDetail(frontend::BuiltinSignature(Builtin::ArrayJoin)),
        .kind = CompletionKind::Method,
    });
  }
  for (const auto& [name, algorithm] : frontend::ArrayAlgorithms()) {
    auto accumulator = element;
    if (algorithm == frontend::ArrayAlgorithm::Reduce ||
        algorithm == frontend::ArrayAlgorithm::Map) {
      accumulator.name = "R";
    }
    result.push_back({
        .label = std::string(name),
        .detail = BuiltinDetail(frontend::ArrayAlgorithmSignature(algorithm, element, accumulator)),
        .kind = CompletionKind::Method,
    });
  }
}

}  // namespace

std::optional<Completion> SemanticIndex::BuiltinCompletion(const frontend::Token& token) const {
  std::vector<Completion> candidates;
  if (const auto receiver = receivers_.find(Key{0, token.offset}); receiver != receivers_.end()) {
    candidates = MemberCompletions(receiver->second);
  } else {
    const auto& tokens = sources_.front().tokens;
    const auto index = static_cast<std::size_t>(&token - tokens.data());
    if (index > 0 && tokens.at(index - 1).kind == frontend::TokenKind::Dot) {
      return std::nullopt;
    }
    AddBuiltins(candidates, Kind::Invalid);
  }
  const auto found = std::ranges::find(candidates, token.lexeme, &Completion::label);
  return found == candidates.end() ? std::nullopt : std::optional(*found);
}

std::vector<Completion> SemanticIndex::MemberCompletions(const Receiver& receiver) const {
  std::vector<Completion> result;
  if (receiver.type.nullable || receiver.type.kind == Kind::Invalid) {
    return result;
  }
  const auto members = members_.find(receiver.type.name);
  if (members != members_.end()) {
    for (const auto& [name, symbol_id] : members->second) {
      const auto& symbol = symbols_.at(symbol_id);
      if (symbol.storage == receiver.storage &&
          (symbol.visibility == frontend::Visibility::Public ||
           receiver.context_class == receiver.type.name)) {
        result.push_back(symbol.completion);
      }
    }
  }
  if (receiver.storage == frontend::MemberStorage::Static) {
    return result;
  }
  AddContainerCompletions(result, receiver.type);
  return result;
}

void SemanticIndex::AddContainerCompletions(std::vector<Completion>& result,
                                            const frontend::TypeDescriptor& type) const {
  const auto kind = type.kind;
  AddBuiltins(result, kind);
  const auto property = [&result](std::string_view name) -> void {
    result.push_back(
        {.label = std::string(name), .detail = "int", .kind = CompletionKind::Property});
  };
  const auto method = [&result](std::string_view name, std::string detail) -> void {
    result.push_back(
        {.label = std::string(name), .detail = std::move(detail), .kind = CompletionKind::Method});
  };
  if (kind == Kind::Array || kind == Kind::String) {
    property("length");
  }
  if (kind == Kind::String) {
    property("byteLength");
    property("graphemeLength");
  } else if (kind == Kind::Array) {
    method("copy", "(): " + type.name);
    method("slice", "(start: int, end: int): " + type.name);
    method("reverse", "(): void");
    const auto element = model_.array_elements.find(type.name);
    if (element != model_.array_elements.end()) {
      AddArrayElementMethods(result, element->second);
    }
  } else if (kind == Kind::Set || kind == Kind::Map || kind == Kind::Expected) {
    const auto arguments = model_.generic_arguments.find(type.name);
    if (arguments == model_.generic_arguments.end()) {
      return;
    }
    const auto& types = arguments->second;
    if (kind == Kind::Expected) {
      method("hasValue", "(): bool");
      method("value", "(): " + types.front().name);
      method("error", "(): " + types.back().name);
    } else {
      property("size");
      const std::string_view parameter = kind == Kind::Set ? "value: " : "key: ";
      method("has", "(" + std::string(parameter) + types.front().name + "): bool");
      method("delete", "(" + std::string(parameter) + types.front().name + "): bool");
      method("clear", "(): void");
      method("values", "(): " + types.back().name + "[]");
      if (kind == Kind::Set) {
        method("add", "(value: " + types.front().name + "): bool");
        method("union", "(other: " + type.name + "): " + type.name);
        method("intersection", "(other: " + type.name + "): " + type.name);
        method("difference", "(other: " + type.name + "): " + type.name);
      } else {
        method("keys", "(): " + types.front().name + "[]");
        method("get", "(key: " + types.front().name + "): " + types.back().name +
                          (types.back().nullable ? "" : "?"));
        method("set", "(key: " + types.front().name + ", value: " + types.back().name + "): void");
      }
    }
  }
}

absl::flat_hash_map<std::string, Completion> SemanticIndex::VisibleLocals(
    std::size_t offset) const {
  std::vector<const Local*> locals;
  for (const auto& local : locals_) {
    const auto& scope = local.scope;
    if (scope.source == 0 && offset >= local.available && offset >= scope.begin &&
        offset < scope.end) {
      locals.push_back(&local);
    }
  }
  std::ranges::sort(locals, [](const Local* left, const Local* right) -> bool {
    return left->scope.depth < right->scope.depth;
  });
  absl::flat_hash_map<std::string, Completion> selected;
  for (const auto* local : locals) {
    const auto& completion = symbols_.at(local->symbol).completion;
    selected.insert_or_assign(completion.label, completion);
  }
  return selected;
}

std::vector<Completion> SemanticIndex::QualifiedCompletions(
    std::size_t token_index,
    const absl::flat_hash_map<std::string, Completion>& locals) const {
  const auto& tokens = sources_.front().tokens;
  const auto& token = tokens.at(token_index);
  if (const auto receiver = receivers_.find(Key{0, token.offset}); receiver != receivers_.end()) {
    return MemberCompletions(receiver->second);
  }
  if (token_index < 2 || tokens.at(token_index - 2).kind != frontend::TokenKind::Identifier) {
    return {};
  }
  const auto& name = tokens.at(token_index - 2).lexeme;
  const auto imported = prefixes_.find(name);
  if (imported == prefixes_.end() || locals.contains(name)) {
    return {};
  }
  std::vector<Completion> result;
  for (const auto symbol_id : exports_.at(imported->second)) {
    result.push_back(symbols_.at(symbol_id).completion);
  }
  return result;
}

std::vector<Completion> SemanticIndex::Complete(std::size_t offset) const {
  if (sources_.empty()) {
    return {};
  }
  const auto& source = sources_.front();
  for (const auto& comment : source.comments) {
    if (offset >= comment.offset && offset <= comment.offset + comment.length) {
      return {};
    }
  }
  const auto& tokens = source.tokens;
  const auto after = std::ranges::upper_bound(tokens, offset, {}, &frontend::Token::offset);
  const auto current =
      after == tokens.begin() ? 0 : static_cast<std::size_t>(std::prev(after) - tokens.begin());
  const auto& token = tokens.at(current);
  const auto prefix = token.kind == frontend::TokenKind::Identifier && offset >= token.offset
                          ? source.text.text().substr(token.offset, offset - token.offset)
                          : std::string_view();
  if (token.kind == frontend::TokenKind::StringLiteral ||
      token.kind == frontend::TokenKind::StringHead ||
      token.kind == frontend::TokenKind::StringMiddle ||
      token.kind == frontend::TokenKind::StringTail) {
    return {};
  }
  std::vector<Completion> result;
  const auto dot = current > 0 && tokens.at(current - 1).kind == frontend::TokenKind::Dot;
  auto selected = VisibleLocals(offset);
  if (dot) {
    result = QualifiedCompletions(current, selected);
  } else {
    for (const auto symbol_id : visible_) {
      const auto& completion = symbols_.at(symbol_id).completion;
      selected.try_emplace(completion.label, completion);
    }
    for (const auto& [name, source_id] : prefixes_) {
      selected.try_emplace(
          name, Completion{.label = name, .detail = "module", .kind = CompletionKind::Module});
    }
    AddBuiltins(result, Kind::Invalid);
    constexpr auto kKeywords = std::to_array<std::string_view>({
        "func",       "let",      "const",   "class",  "enum",    "interface",
        "type",       "public",   "private", "static", "param",   "constructor",
        "implements", "return",   "if",      "else",   "for",     "while",
        "break",      "continue", "switch",  "case",   "default", "match",
        "try",        "catch",    "finally", "throw",  "import",  "export",
        "external",   "is",       "as",      "true",   "false",   "null",
    });
    for (const auto keyword : kKeywords) {
      result.push_back(
          {.label = std::string(keyword), .detail = "keyword", .kind = CompletionKind::Keyword});
    }
    constexpr auto kTypes = std::to_array<std::string_view>({
        "int",
        "double",
        "bool",
        "string",
        "void",
        "Set",
        "Map",
        "Expected",
        "Math",
        "Random",
        "performance",
        "Date",
        "Error",
    });
    for (const auto type : kTypes) {
      result.push_back(
          {.label = std::string(type), .detail = "type", .kind = CompletionKind::Type});
    }
    for (auto& [name, completion] : selected) {
      result.push_back(std::move(completion));
    }
  }
  std::erase_if(result, [prefix](const Completion& completion) -> bool {
    return !completion.label.starts_with(prefix);
  });
  std::ranges::sort(result, {}, &Completion::label);
  const auto duplicates = std::ranges::unique(result, {}, &Completion::label);
  result.erase(duplicates.begin(), duplicates.end());
  return result;
}

}  // namespace vela::tooling
