#include "base/array_algorithm.h"
#include "compiler/tooling/semantic/semantic_index.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include "base/builtin.h"
#include "base/member.h"
#include "compiler/frontend/builtins/async_builtins.h"
#include "compiler/frontend/builtins/builtins.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/syntax/lexer.h"
#include "compiler/frontend/syntax/token.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/frontend/typing/type_names.h"
#include "compiler/tooling/documentation/builtin_documentation.h"
#include "compiler/tooling/semantic/semantic_syntax.h"

namespace vela::tooling {
namespace {

using Kind = frontend::TypeDescriptor::Kind;

void AddPropertyCompletion(std::vector<Completion>& result,
                           std::string_view name,
                           std::string detail) {
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

void AddBuiltins(std::vector<Completion>& result, Kind kind, std::string_view prefix = {}) {
  if (kind == Kind::Invalid) {
    if (frontend::kSleepFunctionName.starts_with(prefix)) {
      result.push_back({
          .label = std::string(frontend::kSleepFunctionName),
          .detail = BuiltinDetail(frontend::DescribeAsyncMember(
              *frontend::FindAsyncMember(Kind::Invalid, frontend::MemberStorage::Static,
                                         frontend::kSleepFunctionName),
              {})),
          .kind = CompletionKind::Function,
          .documentation = std::string(BuiltinDocumentation(kind, frontend::kSleepFunctionName)),
      });
    }
    if (std::string_view("print").starts_with(prefix)) {
      result.push_back({
          .label = "print",
          .detail = "(value): void",
          .kind = CompletionKind::Function,
          .documentation = std::string(BuiltinDocumentation(kind, "print")),
      });
    }
  }
  if (kind == Kind::Invalid) {
    for (const auto type : frontend::kBuiltinTypeNames) {
      if (!type.starts_with(prefix)) {
        continue;
      }
      const auto described = BuiltinTypeDetail(type);
      const auto detail = described.empty() ? std::string_view("type") : described;
      result.push_back({
          .label = std::string(type),
          .detail = std::string(detail),
          .kind = detail.starts_with("class ") ? CompletionKind::Class : CompletionKind::Type,
          .documentation = std::string(BuiltinDocumentation(kind, type)),
      });
    }
    return;
  }
  for (const auto& [name, builtin] : frontend::BuiltinMethods(kind)) {
    if (!name.starts_with(prefix)) {
      continue;
    }
    result.push_back({
        .label = std::string(name),
        .detail = BuiltinDetail(frontend::BuiltinSignature(builtin)),
        .kind = CompletionKind::Method,
        .documentation = std::string(BuiltinDocumentation(kind, name)),
    });
  }
}

void AddArrayElementMethods(std::vector<Completion>& result,
                            const frontend::TypeDescriptor& element,
                            std::string_view prefix) {
  for (const auto& [name, builtin] : frontend::ArrayMethods()) {
    if (!name.starts_with(prefix)) {
      continue;
    }
    result.push_back({
        .label = std::string(name),
        .detail = BuiltinDetail(frontend::ArrayMethodSignature(builtin, element)),
        .kind = CompletionKind::Method,
        .documentation = {},
    });
  }
  if (std::string_view("join").starts_with(prefix) && frontend::CanFormatString(element)) {
    result.push_back({
        .label = "join",
        .detail = BuiltinDetail(frontend::BuiltinSignature(Builtin::ArrayJoin)),
        .kind = CompletionKind::Method,
        .documentation = {},
    });
  }
  for (const auto& [name, algorithm] : frontend::ArrayAlgorithms()) {
    if (!name.starts_with(prefix)) {
      continue;
    }
    auto accumulator = element;
    if (algorithm == ArrayAlgorithm::Reduce || algorithm == ArrayAlgorithm::Map) {
      accumulator.name = "R";
    }
    result.push_back({
        .label = std::string(name),
        .detail = BuiltinDetail(frontend::ArrayAlgorithmSignature(
            algorithm, frontend::ArrayCallbackSignature(algorithm, element, accumulator))),
        .kind = CompletionKind::Method,
        .documentation = {},
    });
  }
}

void AddBuiltinProperties(std::vector<Completion>& result, Kind kind, std::string_view prefix) {
  for (const auto& [name, builtin] : frontend::BuiltinProperties(kind)) {
    if (!name.starts_with(prefix)) {
      continue;
    }
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
    const auto& asynchronous = FactsFor(*call->second).async_calls;
    if (const auto async = asynchronous.find(call->second); async != asynchronous.end()) {
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
    candidates = MemberCompletions(receiver->second, token.lexeme);
  } else {
    const auto& tokens = sources_.front().tokens;
    const auto index = static_cast<std::size_t>(&token - tokens.data());
    if (index > 0 && tokens[index - 1].kind == frontend::TokenKind::Dot) {
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
    AddBuiltins(candidates, Kind::Invalid, token.lexeme);
  }
  const auto found = std::ranges::find(candidates, token.lexeme, &Completion::label);
  return found == candidates.end() ? std::nullopt : std::optional(*found);
}

Completion SemanticIndex::SpecializedCompletion(std::string_view owner,
                                                const Symbol& symbol,
                                                std::string_view name) const {
  auto completion = symbol.completion;
  if (completion.kind != CompletionKind::Property) {
    return completion;
  }
  const auto specialize = [&](const auto& declarations) {
    const auto declaration = declarations.find(owner);
    if (declaration == declarations.end()) {
      return;
    }
    const auto method = declaration->second.methods.find(name);
    if (method == declaration->second.methods.end()) {
      return;
    }
    const auto& signature = method->second;
    if (signature.is_getter) {
      completion.detail = DisplayType(signature.return_type);
    } else if (const auto& property_type = signature.setter_parameter_type) {
      completion.detail = DisplayType(*property_type);
    }
  };
  specialize(model_.classes);
  specialize(model_.interfaces);
  return completion;
}

std::vector<Completion> SemanticIndex::MemberCompletions(const Receiver& receiver,
                                                         std::string_view prefix) const {
  std::vector<Completion> result;
  if (receiver.type.nullable || receiver.type.kind == Kind::Invalid) {
    return result;
  }
  absl::flat_hash_set<std::string> visible;
  for (const auto owner : MemberOwners(receiver.type)) {
    const auto members = members_.find(owner);
    if (members == members_.end()) {
      continue;
    }
    for (const auto& [name, symbol_id] : members->second) {
      if (!name.starts_with(prefix)) {
        continue;
      }
      const auto& symbol = symbols_[symbol_id];
      if (symbol.storage == receiver.storage &&
          (symbol.visibility == frontend::Visibility::Public || receiver.context_class == owner) &&
          visible.insert(symbol.completion.label).second) {
        result.push_back(SpecializedCompletion(owner, symbol, name));
      }
    }
  }
  if (receiver.storage == frontend::MemberStorage::Static) {
    AddAsyncCompletions(result, receiver.type, frontend::MemberStorage::Static, prefix);
  } else {
    AddContainerCompletions(result, receiver.type, prefix);
  }
  for (auto& completion : result) {
    if (completion.documentation.empty()) {
      completion.documentation = BuiltinDocumentation(receiver.type.kind, completion.label);
    }
  }
  return result;
}

void SemanticIndex::AddAsyncCompletions(std::vector<Completion>& result,
                                        const frontend::TypeDescriptor& type,
                                        frontend::MemberStorage storage,
                                        std::string_view prefix) const {
  frontend::TypeDescriptor element{.kind = Kind::Parameter, .name = "T", .is_abstract = true};
  if (storage == frontend::MemberStorage::Instance &&
      (type.kind == Kind::Future || type.kind == Kind::Completer)) {
    const auto found = model_.generic_arguments.find(type.name);
    if (found == model_.generic_arguments.end()) {
      return;
    }
    element = found->second.front();
  }
  for (const auto& member : frontend::AsyncMembers()) {
    if (member.receiver != type.kind || member.storage != storage ||
        !member.name.starts_with(prefix)) {
      continue;
    }
    const auto signature = frontend::DescribeAsyncMember(member, element);
    if (member.kind == MemberKind::Property) {
      AddPropertyCompletion(result, member.name, std::string(signature.return_type.name));
    } else {
      AddMethodCompletion(result, member.name, BuiltinDetail(signature));
    }
  }
}

void SemanticIndex::AddContainerCompletions(std::vector<Completion>& result,
                                            const frontend::TypeDescriptor& type,
                                            std::string_view prefix) const {
  const auto kind = type.kind;
  AddBuiltinProperties(result, kind, prefix);
  AddBuiltins(result, kind, prefix);
  AddAsyncCompletions(result, type, frontend::MemberStorage::Instance, prefix);
  if (kind == Kind::Array) {
    const auto element = model_.array_elements.find(type.name);
    if (element != model_.array_elements.end()) {
      AddArrayElementMethods(result, element->second, prefix);
    }
  } else if (kind == Kind::Set || kind == Kind::Map) {
    const auto arguments = model_.generic_arguments.find(type.name);
    if (arguments == model_.generic_arguments.end()) {
      return;
    }
    for (const auto& [name, builtin] : frontend::CollectionMethods(kind)) {
      if (!name.starts_with(prefix)) {
        continue;
      }
      if (const auto method = frontend::ResolveCollectionBuiltin(type, arguments->second, name)) {
        AddMethodCompletion(result, name, BuiltinDetail(method->signature));
      }
    }
  }
}

absl::flat_hash_map<std::string, Completion> SemanticIndex::VisibleLocals(
    std::size_t offset,
    std::string_view prefix) const {
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
    const auto& completion = symbols_[local->symbol].completion;
    if (completion.label.starts_with(prefix)) {
      selected.insert_or_assign(completion.label, completion);
    }
  }
  return selected;
}

std::vector<Completion> SemanticIndex::QualifiedCompletions(std::size_t token_index,
                                                            std::string_view prefix,
                                                            std::size_t offset) const {
  const auto& tokens = sources_.front().tokens;
  const auto& token = tokens[token_index];
  if (const auto receiver = receivers_.find(Key{0, token.offset}); receiver != receivers_.end()) {
    return MemberCompletions(receiver->second, prefix);
  }
  if (token_index < 2 || tokens[token_index - 2].kind != frontend::TokenKind::Identifier) {
    return {};
  }
  const auto& name = tokens[token_index - 2].lexeme;
  const auto imported = prefixes_.find(name);
  if (imported == prefixes_.end()) {
    return {};
  }
  const auto shadowed = std::ranges::any_of(locals_, [&](const Local& local) {
    const auto& scope = local.scope;
    return scope.source == 0 && offset >= local.available && offset >= scope.begin &&
           offset < scope.end && symbols_[local.symbol].completion.label == name;
  });
  if (shadowed) {
    return {};
  }
  std::vector<Completion> result;
  for (const auto symbol_id : exports_.at(imported->second)) {
    const auto& completion = symbols_[symbol_id].completion;
    if (completion.label.starts_with(prefix)) {
      result.push_back(completion);
    }
  }
  return result;
}

std::vector<Completion> SemanticIndex::ScopeCompletions(std::size_t offset,
                                                        std::string_view prefix) const {
  auto selected = VisibleLocals(offset, prefix);
  for (const auto symbol_id : visible_) {
    const auto& completion = symbols_[symbol_id].completion;
    if (completion.label.starts_with(prefix)) {
      selected.try_emplace(completion.label, completion);
    }
  }
  for (const auto& [name, source_id] : prefixes_) {
    if (name.starts_with(prefix)) {
      selected.try_emplace(name, Completion{
                                     .label = name,
                                     .detail = "module",
                                     .kind = CompletionKind::Module,
                                     .documentation = {},
                                 });
    }
  }
  std::vector<Completion> result;
  AddBuiltins(result, Kind::Invalid, prefix);
  for (auto& completion : result) {
    selected.try_emplace(completion.label, std::move(completion));
  }
  result.clear();
  for (const auto& [keyword, kind] : frontend::Keywords()) {
    if (kind == frontend::TokenKind::Void || !keyword.starts_with(prefix)) {
      continue;
    }
    result.push_back({
        .label = std::string(keyword),
        .detail = "keyword",
        .kind = CompletionKind::Keyword,
        .documentation = {},
    });
  }
  for (auto& [name, completion] : selected) {
    result.push_back(std::move(completion));
  }
  return result;
}

std::vector<Completion> SemanticIndex::Complete(std::size_t offset) const {
  if (sources_.empty()) {
    return {};
  }
  const auto& source = sources_.front();
  if (IsInComment(source.comments, offset)) {
    return {};
  }
  const auto& tokens = source.tokens;
  const auto current = TokenIndexAt(tokens, offset).value_or(0);
  const auto& token = tokens[current];
  const auto prefix = frontend::IsMemberName(token.kind) && offset >= token.offset
                          ? source.text.substr(token.offset, offset - token.offset)
                          : std::string_view();
  if (frontend::IsStringToken(token.kind)) {
    return {};
  }
  std::vector<Completion> result;
  const auto dot = current > 0 && (tokens[current - 1].kind == frontend::TokenKind::Dot ||
                                   tokens[current - 1].kind == frontend::TokenKind::QuestionDot);
  if (dot) {
    result = QualifiedCompletions(current, prefix, offset);
  } else {
    result = ScopeCompletions(offset, prefix);
  }
  std::ranges::sort(result, {}, &Completion::label);
  const auto duplicates = std::ranges::unique(result, {}, &Completion::label);
  result.erase(duplicates.begin(), duplicates.end());
  return result;
}

}  // namespace vela::tooling
