#include "runtime/native_contracts.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "base/declaration.h"
#include "base/layout_description.h"
#include "runtime/native_image.h"

namespace vela::runtime {
namespace {

bool IsSameSignature(const NativeImage& left_image,
                     const NativeSignatureDescription& left,
                     const NativeImage& right_image,
                     const NativeSignatureDescription& right) {
  if (left.has_result != right.has_result || left.parameters.size != right.parameters.size) {
    return false;
  }
  if (left.has_result != 0 &&
      !IsSameNativeType(left_image, left.result, right_image, right.result)) {
    return false;
  }
  const auto left_parameters = left.parameters.values();
  const auto right_parameters = right.parameters.values();
  for (std::size_t index = 0; index < left_parameters.size(); ++index) {
    const auto& first = left_parameters.subspan(index).front();
    const auto& second = right_parameters.subspan(index).front();
    if (first.flags != second.flags ||
        !IsSameNativeType(left_image, first.type, right_image, second.type)) {
      return false;
    }
  }
  return true;
}

bool IsSameTextList(NativeTable<NativeText> left, NativeTable<NativeText> right) {
  return std::ranges::equal(left.values(), right.values(), {}, &NativeText::text,
                            &NativeText::text);
}

using DeclarationIndex = absl::flat_hash_map<DeclarationId, const NativeDeclarationDescription*>;
using DeclarationSlots =
    absl::flat_hash_map<std::pair<DeclarationId, std::string_view>, DeclarationKind>;

std::expected<void, NativeContractViolation> CheckDeclarationContract(
    const NativeDeclarationDescription& declaration,
    const DeclarationIndex& declarations,
    const DeclarationSlots& slots) {
  const auto kind = static_cast<DeclarationKind>(declaration.kind);
  if (kind == DeclarationKind::Field || kind == DeclarationKind::StaticField) {
    const auto matching = slots.find(std::pair(declaration.owner, declaration.name.text()));
    if (matching != slots.end() && kind != matching->second) {
      return std::unexpected(NativeContractViolation{
          .declaration = declaration.identity,
          .message = "field instance/static storage changed; Reload is required",
      });
    }
  }
  const auto found = declarations.find(declaration.identity);
  if (found == declarations.end()) {
    if (kind == DeclarationKind::Class || kind == DeclarationKind::Interface ||
        kind == DeclarationKind::Enum) {
      return std::unexpected(NativeContractViolation{
          .declaration = declaration.identity,
          .message = "nominal type was deleted or changed kind; Reload is required",
      });
    }
  } else if (!IsSameTextList(declaration.type_parameters, found->second->type_parameters)) {
    return std::unexpected(NativeContractViolation{
        .declaration = declaration.identity,
        .message = "generic parameter list changed; Reload is required",
    });
  } else if (!IsSameTextList(declaration.enum_members, found->second->enum_members)) {
    return std::unexpected(NativeContractViolation{
        .declaration = declaration.identity,
        .message = "enum members or their order changed; Reload is required",
    });
  } else if (declaration.generic_signature.text() != found->second->generic_signature.text()) {
    return std::unexpected(NativeContractViolation{
        .declaration = declaration.identity,
        .message = "generic declaration signature changed; Reload is required",
    });
  }
  return {};
}

std::expected<void, NativeContractViolation> CheckFieldContracts(
    const NativeImage& previous_image,
    const NativeClassDescription& previous,
    const NativeImage& candidate_image,
    const NativeClassDescription& candidate) {
  absl::flat_hash_map<DeclarationId, const NativeFieldDescription*> fields;
  for (const auto& field : candidate.fields.values()) {
    fields.emplace(field.declaration, &field);
  }
  for (const auto& field : previous.fields.values()) {
    const auto matching = fields.find(field.declaration);
    if (matching == fields.end()) {
      continue;
    }
    const auto& next = *matching->second;
    if (!IsSameNativeType(previous_image, previous_image.types.values().subspan(field.type).front(),
                          candidate_image,
                          candidate_image.types.values().subspan(next.type).front())) {
      return std::unexpected(NativeContractViolation{
          .declaration = field.declaration,
          .message = "field type or nullability changed; Reload is required",
      });
    }
  }
  return {};
}

std::vector<std::uint64_t> InterfaceSet(const NativeImage& image,
                                        const NativeClassDescription& type) {
  std::vector<std::uint64_t> identities;
  const auto declarations = image.interface_declarations.values();
  for (const auto index : type.interfaces.values()) {
    identities.push_back(declarations.subspan(index).front().value);
  }
  std::ranges::sort(identities);
  return identities;
}

}  // namespace

bool IsSameNativeType(const NativeImage& left_image,
                      const NativeTypeDescription& left,
                      const NativeImage& right_image,
                      const NativeTypeDescription& right) {
  if (left.kind != right.kind || left.nullable != right.nullable) {
    return false;
  }
  using Kind = layout::Type::Kind;
  switch (static_cast<Kind>(left.kind)) {
    case Kind::Instance:
    case Kind::Enum: {
      const auto& first = left_image.classes.values().subspan(left.index).front();
      const auto& second = right_image.classes.values().subspan(right.index).front();
      if (first.declaration != DeclarationId{} || second.declaration != DeclarationId{}) {
        return first.declaration == second.declaration;
      }
      // 固定 runtime 结构由布局种类标识；版本内捕获布局不形成跨版本名义类型。
      const auto kind = static_cast<layout::Class::Kind>(first.kind);
      return first.kind == second.kind &&
             (kind == layout::Class::Kind::RuntimeError || kind == layout::Class::Kind::DateParts);
    }
    case Kind::Interface:
      return left_image.interface_declarations.values().subspan(left.index).front() ==
             right_image.interface_declarations.values().subspan(right.index).front();
    case Kind::Array:
      return IsSameNativeType(
          left_image, left_image.arrays.values().subspan(left.index).front().element, right_image,
          right_image.arrays.values().subspan(right.index).front().element);
    case Kind::Collection: {
      const auto& first = left_image.collections.values().subspan(left.index).front();
      const auto& second = right_image.collections.values().subspan(right.index).front();
      return first.has_value == second.has_value &&
             IsSameNativeType(left_image, first.key, right_image, second.key) &&
             (first.has_value == 0 ||
              IsSameNativeType(left_image, first.value, right_image, second.value));
    }
    case Kind::Callable:
    case Kind::Future:
    case Kind::Completer:
      return IsSameSignature(left_image, left_image.signatures.values().subspan(left.index).front(),
                             right_image,
                             right_image.signatures.values().subspan(right.index).front());
    default:
      return true;
  }
}

std::expected<void, std::string> CheckNativeCallContract(
    const NativeImage& previous_image,
    const NativeFunctionDescription& previous,
    const NativeImage& candidate_image,
    const NativeFunctionDescription& candidate) {
  const auto generated = (candidate.flags & kNativeGeneratedConstructor) != 0;
  const auto was_generated = (previous.flags & kNativeGeneratedConstructor) != 0;
  if (generated != was_generated) {
    return std::unexpected("constructor kind changed; Reload is required");
  }
  if (generated) {
    return {};
  }
  constexpr auto kSignatureFlags = kNativeAsyncFunction | kNativeTypedFunction | kNativeEnumResult;
  if ((previous.flags & kSignatureFlags) != (candidate.flags & kSignatureFlags) ||
      previous.parameter_count != candidate.parameter_count ||
      previous.hidden_parameters != candidate.hidden_parameters ||
      previous.parameters.size != candidate.parameters.size ||
      previous.native_symbol.text() != candidate.native_symbol.text()) {
    return std::unexpected("function calling convention changed; Reload is required");
  }
  const auto previous_types = previous_image.types.values();
  const auto candidate_types = candidate_image.types.values();
  const auto first_result = previous.result_type;
  const auto second_result = candidate.result_type;
  if ((first_result == kNoNativeIndex) != (second_result == kNoNativeIndex) ||
      (first_result != kNoNativeIndex &&
       !IsSameNativeType(previous_image, previous_types.subspan(first_result).front(),
                         candidate_image, candidate_types.subspan(second_result).front()))) {
    return std::unexpected("function return type changed; Reload is required");
  }
  const auto first_parameters = previous.parameters.values();
  const auto second_parameters = candidate.parameters.values();
  for (std::size_t index = 0; index < first_parameters.size(); ++index) {
    const auto& first = first_parameters.subspan(index).front();
    const auto& second = second_parameters.subspan(index).front();
    if (first.name.text() != second.name.text() || first.flags != second.flags ||
        !IsSameNativeType(previous_image, previous_types.subspan(first.type).front(),
                          candidate_image, candidate_types.subspan(second.type).front())) {
      return std::unexpected("function parameter contract changed; Reload is required");
    }
  }
  return {};
}

std::expected<void, NativeContractViolation> CheckNativeNominalContracts(
    const NativeImage& previous,
    const NativeImage& candidate) {
  DeclarationIndex declarations;
  DeclarationSlots slots;
  for (const auto& declaration : candidate.declarations.values()) {
    declarations.emplace(declaration.identity, &declaration);
    const auto kind = static_cast<DeclarationKind>(declaration.kind);
    if (kind == DeclarationKind::Field || kind == DeclarationKind::StaticField) {
      slots.emplace(std::pair(declaration.owner, declaration.name.text()), kind);
    }
  }
  for (const auto& declaration : previous.declarations.values()) {
    if (const auto checked = CheckDeclarationContract(declaration, declarations, slots); !checked) {
      return checked;
    }
  }
  absl::flat_hash_map<DeclarationId, const NativeClassDescription*> classes;
  for (const auto& type : candidate.classes.values()) {
    if (type.declaration != DeclarationId{}) {
      classes.emplace(type.declaration, &type);
    }
  }
  for (const auto& type : previous.classes.values()) {
    const auto found = classes.find(type.declaration);
    if (found == classes.end()) {
      continue;
    }
    const auto& replacement = *found->second;
    if (type.kind != replacement.kind || type.base_class != replacement.base_class ||
        InterfaceSet(previous, type) != InterfaceSet(candidate, replacement)) {
      return std::unexpected(NativeContractViolation{
          .declaration = type.declaration,
          .message = "class kind, superclass or implemented interfaces changed; Reload is required",
      });
    }
    if (const auto fields = CheckFieldContracts(previous, type, candidate, replacement); !fields) {
      return fields;
    }
  }
  return {};
}

}  // namespace vela::runtime
