#pragma once

#include <array>
#include <cstdint>
#include <string_view>
#include <utility>

#include "base/layout_description.h"
#include "vela/types.h"

namespace vela::compiler::host_types {

// 宿主头文件生成器与 Clang 读取器共享源码身份；类型判断在读取边界后使用枚举。
inline constexpr std::string_view kNamespace = "vela";
inline constexpr std::string_view kNamespacePrefix = "vela::";
inline constexpr std::string_view kScriptTraitsName = "ScriptTypeTraits";
inline constexpr std::string_view kScriptKindTypeName = "vela::ScriptTypeKind";
inline constexpr std::string_view kScriptKindMember = "kKind";
inline constexpr std::string_view kScriptNameMember = "kName";
inline constexpr std::string_view kScriptModuleMember = "kModule";
inline constexpr std::string_view kScriptNativeKeyMember = "kNativeKey";
inline constexpr std::string_view kScriptArgumentsMember = "Arguments";

struct ScriptLayoutKind final {
  ScriptTypeKind script;
  layout::Type::Kind type;
  layout::Class::Kind nominal;
};

inline constexpr std::array kScriptLayoutKinds{
    ScriptLayoutKind{
        .script = ScriptTypeKind::Class,
        .type = layout::Type::Kind::Class,
        .nominal = layout::Class::Kind::Instance,
    },
    ScriptLayoutKind{
        .script = ScriptTypeKind::Interface,
        .type = layout::Type::Kind::Interface,
        .nominal = layout::Class::Kind::Instance,
    },
    ScriptLayoutKind{
        .script = ScriptTypeKind::Enum,
        .type = layout::Type::Kind::Enum,
        .nominal = layout::Class::Kind::Enum,
    },
    ScriptLayoutKind{
        .script = ScriptTypeKind::Host,
        .type = layout::Type::Kind::Class,
        .nominal = layout::Class::Kind::Host,
    },
    ScriptLayoutKind{
        .script = ScriptTypeKind::Struct,
        .type = layout::Type::Kind::Class,
        .nominal = layout::Class::Kind::Struct,
    },
};

[[nodiscard]] constexpr const ScriptLayoutKind& LayoutFor(ScriptTypeKind kind) noexcept {
  for (const auto& mapping : kScriptLayoutKinds) {
    if (mapping.script == kind) {
      return mapping;
    }
  }
  std::unreachable();
}

[[nodiscard]] inline ScriptTypeKind ScriptTypeFor(const layout::Program& program,
                                                  const layout::Type& type) {
  auto nominal = layout::Class::Kind::Instance;
  if (type.kind == layout::Type::Kind::Class) {
    nominal = program.classes[type.index].kind;
    if (nominal != layout::Class::Kind::Host && nominal != layout::Class::Kind::Struct) {
      nominal = layout::Class::Kind::Instance;
    }
  }
  for (const auto& mapping : kScriptLayoutKinds) {
    if (mapping.type == type.kind &&
        (type.kind != layout::Type::Kind::Class || mapping.nominal == nominal)) {
      return mapping.script;
    }
  }
  std::unreachable();
}

enum class CppType : std::uint8_t {
  String,
  MutableString,
  ByteArray,
  ByteData,
  RegExp,
  RegExpMatch,
  Timer,
  CancellationToken,
  Value,
  Never,
  Null,
  Local,
  Object,
  Enum,
  Host,
  Nullable,
  Array,
  Map,
  Set,
  Function,
  Future,
  Completer,
};

[[nodiscard]] constexpr std::string_view CppTypeName(CppType type) noexcept {
  switch (type) {
    case CppType::String:
      return "vela::String";
    case CppType::MutableString:
      return "vela::MutableString";
    case CppType::ByteArray:
      return "vela::ByteArray";
    case CppType::ByteData:
      return "vela::ByteData";
    case CppType::RegExp:
      return "vela::RegExp";
    case CppType::RegExpMatch:
      return "vela::RegExpMatch";
    case CppType::Timer:
      return "vela::Timer";
    case CppType::CancellationToken:
      return "vela::CancellationToken";
    case CppType::Value:
      return "vela::Value";
    case CppType::Never:
      return "vela::Never";
    case CppType::Null:
      return "vela::Null";
    case CppType::Local:
      return "vela::Local";
    case CppType::Object:
      return "vela::Object";
    case CppType::Enum:
      return "vela::Enum";
    case CppType::Host:
      return "vela::Host";
    case CppType::Nullable:
      return "vela::Nullable";
    case CppType::Array:
      return "vela::Array";
    case CppType::Map:
      return "vela::Map";
    case CppType::Set:
      return "vela::Set";
    case CppType::Function:
      return "vela::Function";
    case CppType::Future:
      return "vela::Future";
    case CppType::Completer:
      return "vela::Completer";
  }
  std::unreachable();
}

[[nodiscard]] constexpr CppType ScriptWrapper(ScriptTypeKind kind) noexcept {
  switch (kind) {
    case ScriptTypeKind::Enum:
      return CppType::Enum;
    case ScriptTypeKind::Host:
      return CppType::Host;
    case ScriptTypeKind::Class:
    case ScriptTypeKind::Interface:
    case ScriptTypeKind::Struct:
      return CppType::Object;
  }
  std::unreachable();
}

[[nodiscard]] constexpr std::string_view ScriptKindName(ScriptTypeKind kind) noexcept {
  switch (kind) {
    case ScriptTypeKind::Class:
      return "Class";
    case ScriptTypeKind::Interface:
      return "Interface";
    case ScriptTypeKind::Enum:
      return "Enum";
    case ScriptTypeKind::Host:
      return "Host";
    case ScriptTypeKind::Struct:
      return "Struct";
  }
  std::unreachable();
}

}  // namespace vela::compiler::host_types
