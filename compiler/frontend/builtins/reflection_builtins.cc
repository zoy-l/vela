#include "compiler/frontend/builtins/reflection_builtins.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/builtin.h"
#include "base/reflection.h"
#include "base/reflection_operation.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/syntax/ast.h"

namespace vela::frontend {
namespace {

constexpr std::uint8_t kQuery = 1;
constexpr std::uint8_t kTypeArguments = 2;
constexpr std::uint8_t kModule = 4;

struct Binding final {
  std::string_view name;
  reflection::Operation operation;
  std::uint8_t optional_parameters = 0;
};
constexpr auto kBindings = std::to_array<Binding>({
    {.name = "typeOf", .operation = reflection::Operation::TypeOf},
    {.name = "getTypes", .operation = reflection::Operation::GetTypes},
    {
        .name = "findType",
        .operation = reflection::Operation::FindType,
        .optional_parameters = kTypeArguments,
    },
    {.name = "revision", .operation = reflection::Operation::Revision},
    {
        .name = "findMethods",
        .operation = reflection::Operation::FindAttributedMethods,
        .optional_parameters = kModule,
    },
    {.name = "id", .operation = reflection::Operation::Id},
    {.name = "name", .operation = reflection::Operation::Name},
    {.name = "module", .operation = reflection::Operation::Module},
    {.name = "kind", .operation = reflection::Operation::Kind},
    {.name = "isNullable", .operation = reflection::Operation::IsNullable},
    {.name = "isRegistered", .operation = reflection::Operation::IsRegistered},
    {.name = "typeArguments", .operation = reflection::Operation::TypeArguments},
    {.name = "baseType", .operation = reflection::Operation::BaseType},
    {.name = "interfaces", .operation = reflection::Operation::Interfaces},
    {.name = "signature", .operation = reflection::Operation::Signature},
    {.name = "isInstance", .operation = reflection::Operation::IsInstance},
    {.name = "isAssignableFrom", .operation = reflection::Operation::IsAssignableFrom},
    {
        .name = "getFields",
        .operation = reflection::Operation::GetFields,
        .optional_parameters = kQuery,
    },
    {
        .name = "findField",
        .operation = reflection::Operation::FindField,
        .optional_parameters = kQuery,
    },
    {
        .name = "getProperties",
        .operation = reflection::Operation::GetProperties,
        .optional_parameters = kQuery,
    },
    {
        .name = "findProperty",
        .operation = reflection::Operation::FindProperty,
        .optional_parameters = kQuery,
    },
    {
        .name = "getMethods",
        .operation = reflection::Operation::GetMethods,
        .optional_parameters = kQuery,
    },
    {
        .name = "findMethod",
        .operation = reflection::Operation::FindMethod,
        .optional_parameters = kQuery | kTypeArguments,
    },
    {.name = "getConstructor", .operation = reflection::Operation::GetConstructor},
    {.name = "getEnumMembers", .operation = reflection::Operation::GetEnumMembers},
    {.name = "findEnumMember", .operation = reflection::Operation::FindEnumMember},
    {.name = "declaringType", .operation = reflection::Operation::DeclaringType},
    {.name = "visibility", .operation = reflection::Operation::Visibility},
    {.name = "isStatic", .operation = reflection::Operation::IsStatic},
    {.name = "type", .operation = reflection::Operation::ValueType},
    {.name = "canRead", .operation = reflection::Operation::CanRead},
    {.name = "canWrite", .operation = reflection::Operation::CanWrite},
    {.name = "getter", .operation = reflection::Operation::Getter},
    {.name = "setter", .operation = reflection::Operation::Setter},
    {.name = "getValue", .operation = reflection::Operation::GetValue},
    {.name = "setValue", .operation = reflection::Operation::SetValue},
    {.name = "parameters", .operation = reflection::Operation::Parameters},
    {.name = "returnType", .operation = reflection::Operation::ReturnType},
    {.name = "isAsync", .operation = reflection::Operation::IsAsync},
    {.name = "invoke", .operation = reflection::Operation::Invoke},
    {.name = "ordinal", .operation = reflection::Operation::Ordinal},
    {.name = "value", .operation = reflection::Operation::EnumValue},
});

void Bind(Function& function, std::optional<reflection::Operation> operation) {
  std::uint8_t optional_parameters = 0;
  if (operation) {
    function.intrinsic = Builtin::Reflection;
    function.intrinsic_operation = static_cast<std::uint32_t>(*operation);
  } else if (function.name == "typeInfo") {
    function.intrinsic = Builtin::ReflectionTypeInfo;
  } else {
    const auto binding = std::ranges::find(kBindings, function.name, &Binding::name);
    if (binding == kBindings.end()) {
      return;
    }
    function.intrinsic = Builtin::Reflection;
    function.intrinsic_operation = static_cast<std::uint32_t>(binding->operation);
    optional_parameters = binding->optional_parameters;
  }
  function.native_symbol.clear();
  constexpr std::array kOptionalNames{
      std::pair{kModule, "module"},
      std::pair{kTypeArguments, "typeArguments"},
      std::pair{kQuery, "query"},
  };
  for (const auto& [flag, name] : kOptionalNames) {
    if ((optional_parameters & flag) != 0) {
      function.optional_parameters.emplace_back(name);
    }
  }
}

}  // namespace

bool IsReflectionStandardName(std::string_view name) noexcept {
  return std::ranges::binary_search(kReflectionStandardNames, name);
}

Diagnostics BindReflectionDeclarations(Program& program) {
  static_assert(static_cast<std::uint32_t>(reflection::Operation::FindAttributedMethods) ==
                static_cast<std::uint32_t>(reflection::Operation::RecordItem) +
                    reflection::kRecordSlots);
  Diagnostics diagnostics;
  for (auto& owner : program.classes) {
    const auto record = owner.name == "ParameterInfo" || owner.name == "SignatureInfo" ||
                        owner.name == "SignatureParameterInfo" ||
                        owner.name == "MethodAttributeMatch";
    if (record && owner.methods.size() > reflection::kRecordSlots) {
      diagnostics.push_back({
          .location = owner.location,
          .message = "reflection record exceeds its intrinsic slot range",
          .source_path = {},
      });
      continue;
    }
    if (owner.name == "TypeId" || owner.name == "MemberId") {
      owner.reflection_role = Class::ReflectionRole::Identity;
    } else if (record || owner.name == reflection::kTypeInfoName || owner.name == "MemberInfo" ||
               (owner.base_class && owner.base_class->name == "MemberInfo")) {
      owner.reflection_role = Class::ReflectionRole::Metadata;
    }
    for (std::size_t index = 0; index < owner.methods.size(); ++index) {
      auto& method = owner.methods[index];
      std::optional<reflection::Operation> operation;
      if (record) {
        operation = static_cast<reflection::Operation>(
            static_cast<std::uint32_t>(reflection::Operation::RecordItem) + index);
      } else if (owner.name == "ConstructorInfo" && method.name == "invoke") {
        operation = reflection::Operation::Construct;
      }
      Bind(method, operation);
    }
  }
  return diagnostics;
}

}  // namespace vela::frontend
