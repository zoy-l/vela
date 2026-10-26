#pragma once

#include <cstddef>
#include <cstdint>

namespace vela {

inline constexpr std::size_t kUtcComponentCount = 7;

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
  ArraySlice,
  ArrayFill,
  ArrayReverse,
  SetUnion,
  SetIntersection,
  SetDifference,
  ArrayCapacity,
  ArrayPush,
  ArrayPop,
  ArrayPushMany,
  ArrayShift,
  ArrayUnshift,
  ArraySplice,
  ArrayDefaultCompare,
  ArrayClear,
  ArrayReserve,
  ArrayMutation,
  ArrayCheckMutation,
  StringToLowerCase,
  StringToUpperCase,
  IntParse,
  IntTryParse,
  DoubleParse,
  DoubleTryParse,
  DateParse,
  DateToISOString,
  DateFromUTC,
  DateToUTC,
  NewByteArray,
  ByteArrayFrom,
  ByteArrayLength,
  ByteArraySlice,
  ByteArrayGetIndex,
  ByteArraySetIndex,
  ByteDataView,
  ByteDataLength,
  ByteDataGetInt8,
  ByteDataSetInt8,
  ByteDataGetUint8,
  ByteDataSetUint8,
  ByteDataGetInt16,
  ByteDataSetInt16,
  ByteDataGetUint16,
  ByteDataSetUint16,
  ByteDataGetInt32,
  ByteDataSetInt32,
  ByteDataGetUint32,
  ByteDataSetUint32,
  ByteDataGetInt64,
  ByteDataSetInt64,
  ByteDataGetFloat32,
  ByteDataSetFloat32,
  ByteDataGetFloat64,
  ByteDataSetFloat64,
  Count,
};

[[nodiscard]] constexpr std::size_t BuiltinArity(Builtin builtin) noexcept {
  switch (builtin) {
    case Builtin::NewMutableString:
      return 0;
    case Builtin::ScalarToString:
    case Builtin::NewByteArray:
    case Builtin::ByteArrayFrom:
    case Builtin::ByteArrayLength:
    case Builtin::ByteDataLength:
    case Builtin::StringToLowerCase:
    case Builtin::StringToUpperCase:
    case Builtin::DoubleParse:
    case Builtin::DoubleTryParse:
    case Builtin::DateParse:
    case Builtin::DateToISOString:
    case Builtin::MutableStringToString:
    case Builtin::StringLength:
    case Builtin::StringByteLength:
    case Builtin::NumericToInt:
    case Builtin::NumericToDouble:
    case Builtin::ArrayLength:
    case Builtin::StringGraphemeLength:
    case Builtin::CollectionSize:
    case Builtin::CollectionClear:
    case Builtin::ArrayReverse:
    case Builtin::ArrayCapacity:
    case Builtin::ArrayPop:
    case Builtin::ArrayShift:
    case Builtin::ArrayClear:
    case Builtin::ArrayMutation:
      return 1;
    case Builtin::ArrayJoin:
    case Builtin::ByteArrayGetIndex:
    case Builtin::ByteDataGetInt8:
    case Builtin::ByteDataGetUint8:
    case Builtin::IntParse:
    case Builtin::IntTryParse:
    case Builtin::DateToUTC:
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
    case Builtin::SetUnion:
    case Builtin::SetIntersection:
    case Builtin::SetDifference:
    case Builtin::ArrayPush:
    case Builtin::ArrayPushMany:
    case Builtin::ArrayUnshift:
    case Builtin::ArrayDefaultCompare:
    case Builtin::ArrayReserve:
    case Builtin::ArrayCheckMutation:
      return 2;
    case Builtin::StringSlice:
    case Builtin::ByteArraySlice:
    case Builtin::ByteArraySetIndex:
    case Builtin::ByteDataView:
    case Builtin::ByteDataSetInt8:
    case Builtin::ByteDataSetUint8:
    case Builtin::ByteDataGetInt16:
    case Builtin::ByteDataGetUint16:
    case Builtin::ByteDataGetInt32:
    case Builtin::ByteDataGetUint32:
    case Builtin::ByteDataGetInt64:
    case Builtin::ByteDataGetFloat32:
    case Builtin::ByteDataGetFloat64:
    case Builtin::StringGraphemeSlice:
    case Builtin::MapSet:
    case Builtin::ArraySlice:
      return 3;
    case Builtin::ArrayFill:
    case Builtin::ByteDataSetInt16:
    case Builtin::ByteDataSetUint16:
    case Builtin::ByteDataSetInt32:
    case Builtin::ByteDataSetUint32:
    case Builtin::ByteDataSetInt64:
    case Builtin::ByteDataSetFloat32:
    case Builtin::ByteDataSetFloat64:
    case Builtin::ArraySplice:
      return 4;
    case Builtin::DateFromUTC:
      return kUtcComponentCount;
    case Builtin::Count:
      return 0;
  }
  return 0;
}

}  // namespace vela
