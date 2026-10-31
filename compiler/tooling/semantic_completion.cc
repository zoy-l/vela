#include "base/array_algorithm.h"
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
#include "compiler/frontend/byte_builtins.h"
#include "compiler/frontend/regexp_builtins.h"
#include "compiler/frontend/token.h"
#include "compiler/frontend/type_checker.h"
#include "compiler/frontend/type_names.h"
#include "compiler/tooling/builtin_documentation.h"

namespace vela::tooling {
namespace {

using Kind = frontend::TypeDescriptor::Kind;

void AddPropertyCompletion(std::vector<Completion>& result,
                           std::string_view name,
                           std::string detail = std::string(frontend::kIntTypeName)) {
  result.push_back({
      .label = std::string(name),
      .detail = std::move(detail),
      .kind = CompletionKind::Property,
      .documentation = {},
  });
}

void AddMethodCompletion(std::vector<Completion>& result,
                         std::string_view name,
                         std::string detail) {
  result.push_back({
      .label = std::string(name),
      .detail = std::move(detail),
      .kind = CompletionKind::Method,
      .documentation = {},
  });
}

void AddBuiltins(std::vector<Completion>& result, Kind kind) {
  if (kind == Kind::Invalid) {
    result.push_back({
        .label = std::string(frontend::kSleepFunctionName),
        .detail = "(milliseconds: int): Future<void>",
        .kind = CompletionKind::Function,
        .documentation = std::string(BuiltinDocumentation(kind, frontend::kSleepFunctionName)),
    });
    result.push_back({
        .label = "print",
        .detail = "(value): void",
        .kind = CompletionKind::Function,
        .documentation = std::string(BuiltinDocumentation(kind, "print")),
    });
  }
  constexpr auto kNames = std::to_array<std::string_view>({
      frontend::kMutableStringTypeName,
      frontend::kRegExpTypeName,
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
      "toLowerCase",
      "toUpperCase",
  });
  for (const auto name : kNames) {
    if (const auto builtin = frontend::FindBuiltin(kind, name)) {
      auto completion_kind =
          kind == Kind::Invalid ? CompletionKind::Function : CompletionKind::Method;
      if (*builtin == Builtin::NewMutableString || *builtin == Builtin::NewRegExp) {
        completion_kind = CompletionKind::Class;
      }
      result.push_back({
          .label = std::string(name),
          .detail = completion_kind == CompletionKind::Class
                        ? std::string(BuiltinTypeDetail(name))
                        : BuiltinDetail(frontend::BuiltinSignature(*builtin)),
          .kind = completion_kind,
          .documentation = std::string(BuiltinDocumentation(kind, name)),
      });
    }
  }
}

void AddArrayElementMethods(std::vector<Completion>& result,
                            const frontend::TypeDescriptor& element) {
  for (const auto& [name, builtin] : frontend::ArrayMethods()) {
    result.push_back({
        .label = std::string(name),
        .detail = BuiltinDetail(frontend::ArrayMethodSignature(builtin, element)),
        .kind = CompletionKind::Method,
        .documentation = {},
    });
  }
  if (frontend::CanFormatString(element)) {
    result.push_back({
        .label = "join",
        .detail = BuiltinDetail(frontend::BuiltinSignature(Builtin::ArrayJoin)),
        .kind = CompletionKind::Method,
        .documentation = {},
    });
  }
  for (const auto& [name, algorithm] : frontend::ArrayAlgorithms()) {
    auto accumulator = element;
    if (algorithm == ArrayAlgorithm::Reduce || algorithm == ArrayAlgorithm::Map) {
      accumulator.name = "R";
    }
    result.push_back({
        .label = std::string(name),
        .detail = BuiltinDetail(frontend::ArrayAlgorithmSignature(algorithm, element, accumulator)),
        .kind = CompletionKind::Method,
        .documentation = {},
    });
  }
}

void AddByteCompletions(std::vector<Completion>& result, Kind kind) {
  for (const auto& [name, builtin] : frontend::ByteMethods(kind)) {
    result.push_back({
        .label = std::string(name),
        .detail = BuiltinDetail(frontend::BuiltinSignature(builtin)),
        .kind = CompletionKind::Method,
        .documentation = {},
    });
  }
  result.push_back({
      .label = kind == Kind::ByteArray ? "length" : "byteLength",
      .detail = std::string(frontend::kIntTypeName),
      .kind = CompletionKind::Property,
      .documentation = {},
  });
}

void AddRegExpCompletions(std::vector<Completion>& result, Kind kind) {
  for (const auto& [name, builtin] : frontend::RegExpMethods(kind)) {
    result.push_back({
        .label = std::string(name),
        .detail = BuiltinDetail(frontend::BuiltinSignature(builtin)),
        .kind = CompletionKind::Method,
        .documentation = std::string(BuiltinDocumentation(kind, name)),
    });
  }
  for (const auto& [name, builtin] : frontend::RegExpProperties(kind)) {
    result.push_back({
        .label = std::string(name),
        .detail = std::string(frontend::BuiltinSignature(builtin).return_type.name),
        .kind = CompletionKind::Property,
        .documentation = std::string(BuiltinDocumentation(kind, name)),
    });
  }
}

}  // namespace

std::optional<Completion> SemanticIndex::BuiltinCompletion(const frontend::Token& token) const {
  if (const auto call = calls_.find(Key{0, token.offset}); call != calls_.end()) {
    if (const auto async = model_.async_calls.find(call->second);
        async != model_.async_calls.end()) {
      const auto& callee = *call->second->callee;
      const auto receiver =
          async->second.receiver ? ReceiverFor(*callee.object).type.kind : Kind::Invalid;
      return Completion{
          .label = token.lexeme,
          .detail = BuiltinDetail(async->second.signature),
          .kind = callee.kind == frontend::Expression::Kind::Member ? CompletionKind::Method
                                                                    : CompletionKind::Function,
          .documentation = std::string(BuiltinDocumentation(receiver, token.lexeme)),
      };
    }
  }
  std::vector<Completion> candidates;
  if (const auto receiver = receivers_.find(Key{0, token.offset}); receiver != receivers_.end()) {
    candidates = MemberCompletions(receiver->second);
  } else {
    const auto& tokens = sources_.front().tokens;
    const auto index = static_cast<std::size_t>(&token - tokens.data());
    if (index > 0 && tokens.at(index - 1).kind == frontend::TokenKind::Dot) {
      return std::nullopt;
    }
    if (const auto detail = BuiltinTypeDetail(token.lexeme); !detail.empty()) {
      return Completion{
          .label = token.lexeme,
          .detail = std::string(detail),
          .kind = detail.starts_with("class ") ? CompletionKind::Class : CompletionKind::Type,
          .documentation = std::string(BuiltinDocumentation(Kind::Invalid, token.lexeme)),
      };
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
    if (receiver.type.name == frontend::kFutureTypeName) {
      AddMethodCompletion(result, "value", "(value: T): Future<T>");
      AddMethodCompletion(result, "error", "(error: Error): Future<T>");
      AddMethodCompletion(result, "sync", "(computation: () => T): Future<T>");
      AddMethodCompletion(result, "microtask", "(computation: () => T): Future<T>");
      AddMethodCompletion(result, "delayed",
                          "(milliseconds: int, computation: () => T): Future<T>");
      AddMethodCompletion(
          result, "wait",
          "(values: Future<T>[], eagerError?: bool, cleanUp?: ((T) => void)?): Future<T[]>");
      AddMethodCompletion(result, "race", "(values: Future<T>[]): Future<T>");
    } else if (receiver.type.kind == Kind::Timer) {
      AddMethodCompletion(result, "after", "(milliseconds: int, callback: () => void): Timer");
      AddMethodCompletion(result, "periodic", "(milliseconds: int, callback: () => void): Timer");
    }
    for (auto& completion : result) {
      if (completion.documentation.empty()) {
        completion.documentation = BuiltinDocumentation(receiver.type.kind, completion.label);
      }
    }
    return result;
  }
  AddContainerCompletions(result, receiver.type);
  for (auto& completion : result) {
    if (completion.documentation.empty()) {
      completion.documentation = BuiltinDocumentation(receiver.type.kind, completion.label);
    }
  }
  return result;
}

void SemanticIndex::AddAsyncCompletions(std::vector<Completion>& result,
                                        const frontend::TypeDescriptor& type) const {
  const auto kind = type.kind;
  if (kind == Kind::Future || kind == Kind::Completer) {
    const auto found = model_.generic_arguments.find(type.name);
    if (found == model_.generic_arguments.end()) {
      return;
    }
    const auto& element = found->second.front();
    const auto element_name = std::string(element.name);
    const auto type_name = std::string(type.name);
    if (kind == Kind::Completer) {
      AddPropertyCompletion(result, "future", "Future<" + element_name + ">");
      AddPropertyCompletion(result, "isCompleted", std::string(frontend::kBoolTypeName));
      AddMethodCompletion(
          result, "complete",
          element.kind == Kind::Void ? "(): void" : "(value: " + element_name + "): void");
      AddMethodCompletion(result, "completeError", "(error: Error): void");
    } else {
      const auto on_value = element.kind == Kind::Void ? "()" : "(" + element_name + ")";
      AddMethodCompletion(result, "then",
                          "(onValue: " + on_value + " => R, onError?: ((Error) => R)?): Future<R>");
      AddMethodCompletion(result, "catchError",
                          "(onError: (Error) => " + element_name + "): " + type_name);
      AddMethodCompletion(result, "whenComplete", "(action: () => void): " + type_name);
      AddMethodCompletion(
          result, "timeout",
          "(milliseconds: int, onTimeout?: (() => " + element_name + ")?): " + type_name);
    }
  } else if (kind == Kind::Timer || kind == Kind::CancellationToken) {
    AddMethodCompletion(result, "cancel", "(): void");
    if (kind == Kind::CancellationToken) {
      AddPropertyCompletion(result, "isCancelled", std::string(frontend::kBoolTypeName));
    }
  }
}

void SemanticIndex::AddContainerCompletions(std::vector<Completion>& result,
                                            const frontend::TypeDescriptor& type) const {
  const auto kind = type.kind;
  AddRegExpCompletions(result, kind);
  if (!frontend::ByteMethods(kind).empty()) {
    AddByteCompletions(result, kind);
    return;
  }
  AddBuiltins(result, kind);
  AddAsyncCompletions(result, type);
  if (kind == Kind::Array || kind == Kind::String) {
    AddPropertyCompletion(result, "length");
  }
  if (kind == Kind::String) {
    AddPropertyCompletion(result, "byteLength");
    AddPropertyCompletion(result, "graphemeLength");
  } else if (kind == Kind::Array) {
    AddPropertyCompletion(result, "capacity");
    const auto element = model_.array_elements.find(type.name);
    if (element != model_.array_elements.end()) {
      AddArrayElementMethods(result, element->second);
    }
  } else if (kind == Kind::Set || kind == Kind::Map) {
    const auto arguments = model_.generic_arguments.find(type.name);
    if (arguments == model_.generic_arguments.end()) {
      return;
    }
    const auto& types = arguments->second;
    const auto key_name = std::string(types.front().name);
    const auto value_name = std::string(types.back().name);
    const auto collection_name = std::string(type.name);
    AddPropertyCompletion(result, "size");
    const std::string_view parameter = kind == Kind::Set ? "value: " : "key: ";
    AddMethodCompletion(result, "has", "(" + std::string(parameter) + key_name + "): bool");
    AddMethodCompletion(result, "delete", "(" + std::string(parameter) + key_name + "): bool");
    AddMethodCompletion(result, "clear", "(): void");
    AddMethodCompletion(result, "values", "(): " + value_name + "[]");
    if (kind == Kind::Set) {
      AddMethodCompletion(result, "add", "(value: " + key_name + "): bool");
      AddMethodCompletion(result, "union", "(other: " + collection_name + "): " + collection_name);
      AddMethodCompletion(result, "intersection",
                          "(other: " + collection_name + "): " + collection_name);
      AddMethodCompletion(result, "difference",
                          "(other: " + collection_name + "): " + collection_name);
    } else {
      AddMethodCompletion(result, "keys", "(): " + key_name + "[]");
      AddMethodCompletion(
          result, "get",
          "(key: " + key_name + "): " + value_name + (types.back().nullable ? "" : "?"));
      AddMethodCompletion(result, "set",
                          "(key: " + key_name + ", value: " + value_name + "): void");
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
  const auto prefix = frontend::IsMemberName(token.kind) && offset >= token.offset
                          ? source.text.text().substr(token.offset, offset - token.offset)
                          : std::string_view();
  if (token.kind == frontend::TokenKind::StringLiteral ||
      token.kind == frontend::TokenKind::StringHead ||
      token.kind == frontend::TokenKind::StringMiddle ||
      token.kind == frontend::TokenKind::StringTail) {
    return {};
  }
  std::vector<Completion> result;
  const auto dot = current > 0 && (tokens.at(current - 1).kind == frontend::TokenKind::Dot ||
                                   tokens.at(current - 1).kind == frontend::TokenKind::QuestionDot);
  auto selected = VisibleLocals(offset);
  if (dot) {
    result = QualifiedCompletions(current, selected);
  } else {
    for (const auto symbol_id : visible_) {
      const auto& completion = symbols_.at(symbol_id).completion;
      selected.try_emplace(completion.label, completion);
    }
    for (const auto& [name, source_id] : prefixes_) {
      selected.try_emplace(name, Completion{
                                     .label = name,
                                     .detail = "module",
                                     .kind = CompletionKind::Module,
                                     .documentation = {},
                                 });
    }
    AddBuiltins(result, Kind::Invalid);
    constexpr auto kKeywords = std::to_array<std::string_view>({
        "func",    "let",     "const",   "class",       "enum",       "interface",
        "type",    "private", "static",  "constructor", "implements", "return",
        "if",      "else",    "for",     "while",       "break",      "continue",
        "switch",  "case",    "default", "match",       "try",        "catch",
        "finally", "throw",   "import",  "export",      "external",   "is",
        "as",      "true",    "false",   "null",        "async",      "await",
    });
    for (const auto keyword : kKeywords) {
      result.push_back({
          .label = std::string(keyword),
          .detail = "keyword",
          .kind = CompletionKind::Keyword,
          .documentation = {},
      });
    }
    constexpr auto kTypes = std::to_array<std::string_view>({
        frontend::kDynamicTypeName,
        frontend::kIntTypeName,
        frontend::kDoubleTypeName,
        frontend::kBoolTypeName,
        frontend::kStringTypeName,
        frontend::kVoidTypeName,
        frontend::kSetTypeName,
        frontend::kMapTypeName,
        frontend::kRegExpMatchTypeName,
        frontend::kFutureTypeName,
        frontend::kCompleterTypeName,
        frontend::kTimerTypeName,
        frontend::kCancellationTokenTypeName,
        frontend::kTimeoutExceptionTypeName,
        frontend::kMathTypeName,
        frontend::kRandomTypeName,
        frontend::kPerformanceTypeName,
        frontend::kDateTypeName,
        frontend::kJsonTypeName,
        frontend::kErrorTypeName,
    });
    for (const auto type : kTypes) {
      selected.try_emplace(
          std::string(type),
          Completion{
              .label = std::string(type),
              .detail = "type",
              .kind = CompletionKind::Type,
              .documentation = std::string(BuiltinDocumentation(Kind::Invalid, type)),
          });
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
