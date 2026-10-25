#include "compiler/tooling/semantic_index.h"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>

#include "compiler/tooling/source_text.h"

namespace vela::tooling {

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
    const auto& declaration = symbols_.at(symbol->second);
    completion = declaration.completion;
    owner = declaration.owner;
    role = declaration.role;
  } else if (const auto builtin = BuiltinCompletion(*token)) {
    completion = *builtin;
  } else {
    return std::nullopt;
  }
  if (const auto receiver = receivers_.find(Key{0, token->offset}); receiver != receivers_.end()) {
    owner = DisplaySymbolName(receiver->second.type.name);
  }
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
    case CompletionKind::Enum:
      signature = completion.detail + ' ' + signature;
      break;
    case CompletionKind::Type:
      if (completion.detail.starts_with("type ")) {
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
  const auto& source = sources_.front().text;
  return Hover{
      .signature = std::move(signature),
      .documentation = std::move(completion.documentation),
      .range =
          {
              .start = source.PositionAt(token->offset, encoding),
              .end = source.PositionAt(token->offset + token->length, encoding),
          },
  };
}

}  // namespace vela::tooling
