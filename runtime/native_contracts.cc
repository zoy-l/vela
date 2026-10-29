#include "runtime/native_contracts.h"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <string>
#include <vector>

#include <absl/container/flat_hash_map.h>

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
    const auto& first = left_parameters[index];
    const auto& second = right_parameters[index];
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

std::vector<std::uint64_t> InterfaceSet(const NativeImage& image,
                                        const NativeClassDescription& type) {
  std::vector<std::uint64_t> identities;
  const auto declarations = image.interface_declarations.values();
  for (const auto index : type.interfaces.values()) {
    identities.push_back(declarations[index].value);
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
      const auto& first = left_image.classes.values()[left.index];
      const auto& second = right_image.classes.values()[right.index];
      if (first.declaration != DeclarationId{} || second.declaration != DeclarationId{}) {
        return first.declaration == second.declaration;
      }
      // 固定 runtime 结构由布局种类标识；版本内捕获布局不形成跨版本名义类型。
      const auto kind = static_cast<layout::Class::Kind>(first.kind);
      return first.kind == second.kind &&
             (kind == layout::Class::Kind::RuntimeError || kind == layout::Class::Kind::DateParts);
    }
    case Kind::Interface:
      return left_image.interface_declarations.values()[left.index] ==
             right_image.interface_declarations.values()[right.index];
    case Kind::Array:
      return IsSameNativeType(left_image, left_image.arrays.values()[left.index].element,
                              right_image, right_image.arrays.values()[right.index].element);
    case Kind::Collection: {
      const auto& first = left_image.collections.values()[left.index];
      const auto& second = right_image.collections.values()[right.index];
      return first.has_value == second.has_value &&
             IsSameNativeType(left_image, first.key, right_image, second.key) &&
             (first.has_value == 0 ||
              IsSameNativeType(left_image, first.value, right_image, second.value));
    }
    case Kind::Callable:
    case Kind::Expected:
    case Kind::Future:
    case Kind::Completer:
      return IsSameSignature(left_image, left_image.signatures.values()[left.index], right_image,
                             right_image.signatures.values()[right.index]);
    default:
      return true;
  }
}

std::expected<void, std::string> CheckNativeCallContract(
    const NativeImage& previous_image,
    const NativeFunctionDescription& previous,
    const NativeImage& candidate_image,
    const NativeFunctionDescription& candidate) {
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
       !IsSameNativeType(previous_image, previous_types[first_result], candidate_image,
                         candidate_types[second_result]))) {
    return std::unexpected("function return type changed; Reload is required");
  }
  const auto first_parameters = previous.parameters.values();
  const auto second_parameters = candidate.parameters.values();
  for (std::size_t index = 0; index < first_parameters.size(); ++index) {
    const auto& first = first_parameters[index];
    const auto& second = second_parameters[index];
    if (first.name.text() != second.name.text() || first.flags != second.flags ||
        !IsSameNativeType(previous_image, previous_types[first.type], candidate_image,
                          candidate_types[second.type])) {
      return std::unexpected("function parameter contract changed; Reload is required");
    }
  }
  return {};
}

std::expected<void, NativeContractViolation> CheckNativeNominalContracts(
    const NativeImage& previous,
    const NativeImage& candidate) {
  absl::flat_hash_map<DeclarationId, const NativeDeclarationDescription*> declarations;
  for (const auto& declaration : candidate.declarations.values()) {
    declarations.emplace(declaration.identity, &declaration);
  }
  for (const auto& declaration : previous.declarations.values()) {
    const auto found = declarations.find(declaration.identity);
    if (found == declarations.end()) {
      const auto kind = static_cast<DeclarationKind>(declaration.kind);
      if (kind == DeclarationKind::Class || kind == DeclarationKind::Interface ||
          kind == DeclarationKind::Enum) {
        return std::unexpected(NativeContractViolation{
            .declaration = declaration.identity,
            .message = "nominal type was deleted or changed kind; Reload is required"});
      }
    } else if (!IsSameTextList(declaration.type_parameters, found->second->type_parameters)) {
      return std::unexpected(
          NativeContractViolation{.declaration = declaration.identity,
                                  .message = "generic parameter list changed; Reload is required"});
    } else if (!IsSameTextList(declaration.enum_members, found->second->enum_members)) {
      return std::unexpected(NativeContractViolation{
          .declaration = declaration.identity,
          .message = "enum members or their order changed; Reload is required"});
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
          .message =
              "class kind, superclass or implemented interfaces changed; Reload is required"});
    }
  }
  return {};
}

}  // namespace vela::runtime
