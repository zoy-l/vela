#pragma once

#include <cstddef>
#include <cstdint>

namespace vela {

// 编译器与 VM 共享的内建调用编号，不携带前端或运行时对象。
enum class Builtin : std::uint8_t {
  ScalarToString,
  ArrayJoin,
  NewMutableString,
  MutableStringAppend,
  MutableStringToString,
  StringLength,
  StringByteLength,
  StringSlice,
  StringIndexOf,
  StringIncludes,
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
  CollectionSize,
  SetAdd,
  CollectionHas,
  CollectionDelete,
  CollectionClear,
  MapGet,
  MapSet,
  CollectionKeys,
  CollectionValues,
  ArrayCopy,
  ArraySlice,
  ArrayFill,
  ArrayReverse,
  SetUnion,
  SetIntersection,
  SetDifference,
  ArrayCapacity,
  ArrayPush,
  ArrayPop,
  ArrayInsert,
  ArrayRemoveAt,
  ArrayClear,
  ArrayReserve,
  ArrayMutation,
  ArrayCheckMutation,
  Count,
};

[[nodiscard]] constexpr std::size_t BuiltinArity(Builtin builtin) noexcept {
  switch (builtin) {
    case Builtin::NewMutableString:
      return 0;
    case Builtin::ScalarToString:
    case Builtin::MutableStringToString:
    case Builtin::StringLength:
    case Builtin::StringByteLength:
    case Builtin::NumericToInt:
    case Builtin::NumericToDouble:
    case Builtin::ArrayLength:
    case Builtin::StringGraphemeLength:
    case Builtin::CollectionSize:
    case Builtin::CollectionClear:
    case Builtin::ArrayCopy:
    case Builtin::ArrayReverse:
    case Builtin::ArrayCapacity:
    case Builtin::ArrayPop:
    case Builtin::ArrayClear:
    case Builtin::ArrayMutation:
      return 1;
    case Builtin::ArrayJoin:
    case Builtin::MutableStringAppend:
    case Builtin::StringIndexOf:
    case Builtin::StringIncludes:
    case Builtin::StringStartsWith:
    case Builtin::StringEndsWith:
    case Builtin::StringIndex:
    case Builtin::StringCodePoints:
    case Builtin::StringGraphemes:
    case Builtin::SetAdd:
    case Builtin::CollectionHas:
    case Builtin::CollectionDelete:
    case Builtin::MapGet:
    case Builtin::CollectionKeys:
    case Builtin::CollectionValues:
    case Builtin::ArrayFill:
    case Builtin::SetUnion:
    case Builtin::SetIntersection:
    case Builtin::SetDifference:
    case Builtin::ArrayPush:
    case Builtin::ArrayRemoveAt:
    case Builtin::ArrayReserve:
    case Builtin::ArrayCheckMutation:
      return 2;
    case Builtin::StringSlice:
    case Builtin::StringGraphemeSlice:
    case Builtin::MapSet:
    case Builtin::ArraySlice:
    case Builtin::ArrayInsert:
      return 3;
    case Builtin::Count:
      return 0;
  }
  return 0;
}

}  // namespace vela
