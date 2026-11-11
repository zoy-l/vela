#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "base/callable.h"
#include "base/declaration.h"

namespace vela::reflection {

inline constexpr std::string_view kModule = "builtin:reflection";
inline constexpr std::string_view kConstructorName = "constructor";
inline constexpr std::string_view kAttributeName = "Attribute";
inline constexpr std::string_view kAttributeUsageName = "AttributeUsage";
inline constexpr std::string_view kReflectName = "Reflect";
inline constexpr std::string_view kTypeInfoName = "TypeInfo";

enum class TypeKind : std::uint8_t {
  Void,
  Null,
  Dynamic,
  Bool,
  Int,
  Double,
  String,
  MutableString,
  Array,
  Set,
  Map,
  Function,
  Class,
  Interface,
  Enum,
  Future,
  Completer,
  Timer,
  CancellationToken,
  ByteArray,
  ByteData,
  RegExp,
  RegExpMatch,
};

enum class MemberKind : std::uint8_t {
  Field,
  Property,
  Method,
  Getter,
  Setter,
  Constructor,
  EnumMember,
};

[[nodiscard]] constexpr bool IsCallable(MemberKind kind) noexcept {
  return kind == MemberKind::Method || kind == MemberKind::Getter || kind == MemberKind::Setter ||
         kind == MemberKind::Constructor;
}

enum class MemberScope : std::uint8_t { Declared, IncludeInherited };
enum class MemberStorage : std::uint8_t { Instance, Static, All };
enum class Visibility : std::uint8_t { Public, Private };
enum class VisibilityFilter : std::uint8_t { Public, Private, All };

struct MemberQuery final {
  MemberScope scope = MemberScope::IncludeInherited;
  MemberStorage storage = MemberStorage::All;
  VisibilityFilter visibility = VisibilityFilter::Public;
};

inline constexpr MemberQuery kConstructorQuery{.visibility = VisibilityFilter::All};

inline constexpr std::uint32_t kRegistered = 1;
inline constexpr std::uint32_t kPrivate = 2;
inline constexpr std::uint32_t kStatic = 4;
inline constexpr std::uint32_t kAsync = 8;

// 逻辑签名描述源码调用，不包含接收者、构造 presence 位或机器 ABI 参数。
struct Member final {
  DeclarationId declaration;
  std::uint32_t declaring_type = 0;
  std::string name;
  MemberKind kind = MemberKind::Field;
  std::uint32_t flags = 0;
  std::uint32_t type = kInvalidFunctionTarget;
  std::uint32_t target = kInvalidFunctionTarget;
  DeclarationId getter;
  DeclarationId setter;
  CallSignature signature;
  std::vector<std::uint32_t> default_parameters;
  std::vector<std::uint32_t> type_arguments;
};

struct Type final {
  DeclarationId declaration;
  std::uint32_t type = 0;
  std::string module;
  std::string name;
  std::vector<std::uint32_t> arguments;
  std::uint32_t base = kInvalidFunctionTarget;
  std::vector<std::uint32_t> interfaces;
  std::uint32_t flags = 0;
};

using AttributeScalar = std::variant<std::monostate, std::int64_t, double, bool, std::string>;

struct AttributeValue final {
  std::uint32_t type = 0;
  AttributeScalar value;
};

struct Attribute final {
  DeclarationId target;
  std::uint32_t type = 0;
  std::vector<AttributeValue> fields;
};

}  // namespace vela::reflection
