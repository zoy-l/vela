#include "runtime/native/native_type.h"

#include <algorithm>
#include <expected>
#include <string>
#include <string_view>

#include "base/execution_status.h"
#include "base/layout_description.h"
#include "runtime/isolate/isolate.h"
#include "runtime/isolate/layout_transaction.h"
#include "runtime/isolate/program_layouts.h"
#include "runtime/value/object.h"

namespace vela::runtime {

const layout::Program& NativeType::layouts() const noexcept {
  static const layout::Program kEmpty;
  return description == nullptr ? kEmpty : *description;
}

bool NativeType::IsHost() const noexcept {
  return type && type->kind == layout::Type::Kind::Instance &&
         layouts().classes[type->index].kind == layout::Class::Kind::Host;
}

std::string_view NativeType::host_key() const noexcept {
  return type && IsHost() ? std::string_view(layouts().classes[type->index].native_key)
                          : std::string_view();
}

const void* NativeType::cpp_identity() const noexcept {
  const auto key = host_key();
  const auto found = std::ranges::find(hosts, key, &NativeHostIdentity::key);
  return found == hosts.end() ? nullptr : found->identity;
}

std::string NativeType::ContractKey() const {
  return type ? layout::TypeKey(layouts(), *type) : "void";
}

bool NativeType::operator==(const NativeType& other) const {
  return type.has_value() == other.type.has_value() &&
         (!type || layout::IsSameType(layouts(), *type, other.layouts(), *other.type));
}

std::expected<TypeConstraint, ExecutionStatus> ResolveBuiltinNativeType(Isolate& isolate,
                                                                        NativeType type) {
  const auto& source = type.layouts();
  if (!type.type || !source.interfaces.empty()) {
    return std::unexpected(ExecutionStatus::InvalidEntry);
  }
  for (const auto& nominal : source.classes) {
    if (nominal.kind != layout::Class::Kind::Host ||
        isolate.FindNativeClass(nominal.native_key) == nullptr) {
      return std::unexpected(ExecutionStatus::InvalidEntry);
    }
  }
  LayoutTransaction transaction(isolate, LayoutLifetime::Collectible);
  LayoutCache cache;
  const auto result = InstantiateLayoutType(source, *type.type, isolate, cache);
  transaction.CommitStructural();
  return result;
}

}  // namespace vela::runtime
