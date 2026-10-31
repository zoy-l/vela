#include "compiler/tooling/semantic/semantic_index.h"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>

#include "compiler/tooling/source_text.h"

namespace vela::tooling {

std::string SemanticIndex::HoverSignature(const Completion& completion,
                                          const std::string& owner,
                                          Symbol::Role role) {
  auto signature = completion.label;
  switch (completion.kind) {
    case CompletionKind::Function:
      signature = "func " + signature + completion.detail;
      break;
    case CompletionKind::Method:
      signature = "(method) " + owner + '.' + signature + completion.detail;
      break;
    case CompletionKind::Class:
      signature = completion.detail;
      break;
    case CompletionKind::Interface:
      signature = completion.detail.starts_with("interface ") ? completion.detail
                                                              : completion.detail + ' ' + signature;
      break;
    case CompletionKind::Enum:
      signature = completion.detail + ' ' + signature;
      break;
    case CompletionKind::Type:
      if (role == Symbol::Role::TypeParameter || completion.detail.starts_with("type ")) {
        signature = completion.detail;
      } else {
        signature =
            "type " + signature + (completion.detail == signature ? "" : " = " + completion.detail);
      }
      break;
    case CompletionKind::Constant:
      signature = "const " + signature + ": " + completion.detail;
      break;
    case CompletionKind::EnumMember:
      signature = "(enum member) " + completion.detail;
      break;
    case CompletionKind::Field:
    case CompletionKind::Property:
      signature = "(property) " + owner + '.' + signature + ": " + completion.detail;
      break;
    default:
      signature = (role == Symbol::Role::Parameter ? "(parameter) " : "") + signature + ": " +
                  completion.detail;
      break;
  }
  return signature;
}

std::optional<Hover> SemanticIndex::FindHover(std::size_t offset, PositionEncoding encoding) const {
  const auto* token = QueryToken(offset);
  if (token == nullptr) {
    return std::nullopt;
  }
  Completion completion;
  std::string owner;
  auto role = Symbol::Role::Ordinary;
  const auto symbol = references_.find(Key{0, token->offset});
  if (symbol != references_.end()) {
    const auto& reference = symbol->second;
    const auto& declaration = symbols_.at(reference.symbol);
    completion = declaration.completion;
    owner = declaration.owner;
    role = declaration.role;
    const auto kind = completion.kind;
    if (kind == CompletionKind::Method && calls_.contains(Key{0, token->offset})) {
      if (const auto signature = FindCallParameters(token->offset)) {
        completion.detail = DisplaySignature(*signature);
      }
    }
    if (reference.expression != nullptr &&
        (kind == CompletionKind::Variable || kind == CompletionKind::Constant ||
         kind == CompletionKind::Field || kind == CompletionKind::Property)) {
      const auto& types = FactsFor(*reference.expression).expression_types;
      const auto type = types.find(reference.expression);
      if (type != types.end() && type->second.kind != frontend::TypeDescriptor::Kind::Invalid) {
        completion.detail = DisplayType(type->second);
      }
    }
  } else if (const auto builtin = BuiltinCompletion(*token)) {
    completion = *builtin;
  } else {
    return std::nullopt;
  }
  if (const auto receiver = receivers_.find(Key{0, token->offset}); receiver != receivers_.end()) {
    owner = DisplayType(receiver->second.type);
  }
  const auto& source = sources_.front().text;
  return Hover{
      .signature = HoverSignature(completion, owner, role),
      .documentation = std::move(completion.documentation),
      .range =
          {
              .start = source.PositionAt(token->offset, encoding),
              .end = source.PositionAt(token->offset + token->length, encoding),
          },
  };
}

}  // namespace vela::tooling
