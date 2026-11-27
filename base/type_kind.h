#pragma once

#include <cstdint>
#include <string_view>
#include <utility>

namespace vela {

// 语言类型身份在语义分析、布局和宿主契约中共享；运行时值形状另行描述。
enum class LanguageTypeKind : std::uint8_t {
  Dynamic,
  Bool,
  Int,
  Double,
  String,
  MutableString,
  ByteArray,
  ByteData,
  RegExp,
  RegExpMatch,
  Class,
  Enum,
  Interface,
  Array,
  Set,
  Function,
  Future,
  Completer,
  Timer,
  CancellationToken,
  Never,
  Null,
  Map,
  Invalid,
  Void,
  // 抽象参数只用于定义检查，不出现在执行布局中。
  Parameter,
};

// 返回语言中内建类型构造器的拼写；名义类型和语法形式没有固定名称。
[[nodiscard]] constexpr std::string_view BuiltinTypeName(LanguageTypeKind kind) noexcept {
  switch (kind) {
    case LanguageTypeKind::Null:
      return "null";
    case LanguageTypeKind::Void:
      return "void";
    case LanguageTypeKind::Never:
      return "never";
    case LanguageTypeKind::Dynamic:
      return "dynamic";
    case LanguageTypeKind::Bool:
      return "bool";
    case LanguageTypeKind::Int:
      return "int";
    case LanguageTypeKind::Double:
      return "double";
    case LanguageTypeKind::String:
      return "string";
    case LanguageTypeKind::MutableString:
      return "String";
    case LanguageTypeKind::ByteArray:
      return "ByteArray";
    case LanguageTypeKind::ByteData:
      return "ByteData";
    case LanguageTypeKind::RegExp:
      return "RegExp";
    case LanguageTypeKind::RegExpMatch:
      return "RegExpMatch";
    case LanguageTypeKind::Future:
      return "Future";
    case LanguageTypeKind::Completer:
      return "Completer";
    case LanguageTypeKind::Timer:
      return "Timer";
    case LanguageTypeKind::CancellationToken:
      return "CancellationToken";
    case LanguageTypeKind::Set:
      return "Set";
    case LanguageTypeKind::Map:
      return "Map";
    case LanguageTypeKind::Invalid:
    case LanguageTypeKind::Array:
    case LanguageTypeKind::Function:
    case LanguageTypeKind::Enum:
    case LanguageTypeKind::Class:
    case LanguageTypeKind::Interface:
    case LanguageTypeKind::Parameter:
      return {};
  }
  std::unreachable();
}

}  // namespace vela
