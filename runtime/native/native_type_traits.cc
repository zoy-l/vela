#include "runtime/native/native_type_traits.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <vela/types.h>
#include "base/layout_description.h"
#include "runtime/native/native_type.h"

namespace vela::runtime::internal {
namespace {

layout::Type Import(NativeTypeBlueprint& target, NativeType source) {
  if (!source.type) {
    std::unreachable();
  }
  for (const auto& host : source.hosts) {
    if (std::ranges::find(target.hosts, host.key, &NativeHostIdentity::key) == target.hosts.end()) {
      target.hosts.push_back(host);
    }
  }
  return layout::ImportType(target.layouts, source.layouts(), *source.type);
}

void NameRoot(NativeTypeBlueprint& description) {
  if (!description.type) {
    std::unreachable();
  }
  const auto type = *description.type;
  const auto name = layout::TypeKey(description.layouts, type);
  using Kind = layout::Type::Kind;
  switch (type.kind) {
    case Kind::Array:
      description.layouts.arrays[type.index].name = name;
      break;
    case Kind::Collection:
      description.layouts.collections[type.index].name = name;
      break;
    case Kind::Callable:
    case Kind::Future:
    case Kind::Completer:
      description.layouts.signatures[type.index].name = name;
      break;
    default:
      std::unreachable();
  }
}

}  // namespace

NativeTypeBlueprint DescribeNativeArray(NativeType element) {
  NativeTypeBlueprint result;
  const auto imported = Import(result, element);
  result.type = layout::Type{
      .kind = layout::Type::Kind::Array,
      .index = static_cast<std::uint32_t>(result.layouts.arrays.size()),
  };
  result.layouts.arrays.push_back({.name = {}, .element = imported});
  NameRoot(result);
  return result;
}

NativeTypeBlueprint DescribeNativeCollection(NativeType key, std::optional<NativeType> value) {
  NativeTypeBlueprint result;
  layout::Collection collection{.name = {}, .key = Import(result, key)};
  if (value) {
    collection.value = Import(result, *value);
  }
  result.type = layout::Type{
      .kind = layout::Type::Kind::Collection,
      .index = static_cast<std::uint32_t>(result.layouts.collections.size()),
  };
  result.layouts.collections.push_back(std::move(collection));
  NameRoot(result);
  return result;
}

NativeTypeBlueprint DescribeNativeSignature(layout::Type::Kind kind,
                                            NativeType result_type,
                                            std::span<const NativeType> parameters) {
  NativeTypeBlueprint result;
  layout::Signature signature{.name = {}, .parameters = {}};
  for (const auto parameter : parameters) {
    signature.parameters.push_back({.type = Import(result, parameter)});
  }
  if (!result_type.IsVoid()) {
    signature.result = Import(result, result_type);
  }
  result.type = layout::Type{
      .kind = kind,
      .index = static_cast<std::uint32_t>(result.layouts.signatures.size()),
  };
  result.layouts.signatures.push_back(std::move(signature));
  NameRoot(result);
  return result;
}

NativeTypeBlueprint DescribeNativeNominal(std::string_view module,
                                          std::string_view name,
                                          ScriptTypeKind kind,
                                          std::span<const NativeType> arguments) {
  NativeTypeBlueprint result;
  layout::Nominal nominal{
      .module = std::string(module),
      .name = std::string(name),
      .arguments = {},
      .exported = true,
  };
  for (const auto argument : arguments) {
    nominal.arguments.push_back(Import(result, argument));
  }
  const auto key = layout::NominalKey(result.layouts, nominal);
  result.layouts.nominals.emplace(key, std::move(nominal));
  using Kind = layout::Type::Kind;
  if (kind == ScriptTypeKind::Interface) {
    result.type = layout::Type{
        .kind = Kind::Interface,
        .index = static_cast<std::uint32_t>(result.layouts.interfaces.size()),
    };
    result.layouts.interfaces.push_back(key);
    return result;
  }
  const bool is_enum = kind == ScriptTypeKind::Enum;
  result.type = layout::Type{
      .kind = is_enum ? Kind::Enum : Kind::Instance,
      .index = static_cast<std::uint32_t>(result.layouts.classes.size()),
  };
  result.layouts.classes.push_back({
      .name = key,
      .native_key = {},
      .native_contract = {},
      .kind = is_enum ? layout::Class::Kind::Enum : layout::Class::Kind::Instance,
      .reference_fields = {},
      .callable = {},
      .interfaces = {},
      .dispatch_targets = {},
  });
  return result;
}

NativeTypeBlueprint DescribeNativeHost(std::string_view key) {
  NativeTypeBlueprint result;
  result.type = layout::Type{.kind = layout::Type::Kind::Instance};
  result.layouts.classes.push_back({
      .name = {},
      .native_key = std::string(key),
      .native_contract = {},
      .kind = layout::Class::Kind::Host,
      .reference_fields = {},
      .callable = {},
      .interfaces = {},
      .dispatch_targets = {},
  });
  return result;
}

}  // namespace vela::runtime::internal
