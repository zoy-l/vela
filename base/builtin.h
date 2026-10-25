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
  NumericToInt,
  NumericToDouble,
  ArrayLength,
  StringCodePoints,
  StringGraphemes,
  StringGraphemeLength,
  StringGraphemeSlice,
  CollectionLength,
  SetAdd,
  CollectionHas,
  CollectionRemove,
  CollectionClear,
  MapGet,
  MapSet,
  CollectionKeys,
  CollectionValues,
  ArrayCopy,
  ArraySlice,
  ArrayFill,
  ArrayReverse,
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
    case Builtin::NumericToInt:
    case Builtin::NumericToDouble:
    case Builtin::ArrayLength:
    case Builtin::StringGraphemeLength:
    case Builtin::CollectionLength:
    case Builtin::CollectionClear:
    case Builtin::ArrayCopy:
    case Builtin::ArrayReverse:
      return 1;
    case Builtin::Join:
    case Builtin::BuilderAppend:
    case Builtin::StringIndexOf:
    case Builtin::StringContains:
    case Builtin::StringStartsWith:
    case Builtin::StringEndsWith:
    case Builtin::StringIndex:
    case Builtin::StringCodePoints:
    case Builtin::StringGraphemes:
    case Builtin::SetAdd:
    case Builtin::CollectionHas:
    case Builtin::CollectionRemove:
    case Builtin::MapGet:
    case Builtin::CollectionKeys:
    case Builtin::CollectionValues:
    case Builtin::ArrayFill:
      return 2;
    case Builtin::StringSlice:
    case Builtin::StringGraphemeSlice:
    case Builtin::MapSet:
    case Builtin::ArraySlice:
      return 3;
    case Builtin::Count:
      return 0;
  }
  return 0;
}

}  // namespace vela
