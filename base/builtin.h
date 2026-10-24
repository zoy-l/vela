#pragma once

#include <cstddef>
#include <cstdint>

namespace vela {

// 编译器与 VM 共享的内建调用编号，不携带前端或运行时对象。
enum class Builtin : std::uint8_t {
  ScalarToString,
  Join,
  NewStringBuilder,
  BuilderAppend,
  BuilderToString,
  StringLength,
  StringByteLength,
  StringSlice,
  StringIndexOf,
  StringContains,
  StringStartsWith,
  StringEndsWith,
  StringIndex,
  Count,
};

[[nodiscard]] constexpr auto BuiltinArity(Builtin builtin) noexcept -> std::size_t {
  switch (builtin) {
    case Builtin::NewStringBuilder:
      return 0;
    case Builtin::ScalarToString:
    case Builtin::BuilderToString:
    case Builtin::StringLength:
    case Builtin::StringByteLength:
      return 1;
    case Builtin::Join:
    case Builtin::BuilderAppend:
    case Builtin::StringIndexOf:
    case Builtin::StringContains:
    case Builtin::StringStartsWith:
    case Builtin::StringEndsWith:
    case Builtin::StringIndex:
      return 2;
    case Builtin::StringSlice:
      return 3;
    case Builtin::Count:
      return 0;
  }
  return 0;
}

}  // namespace vela
