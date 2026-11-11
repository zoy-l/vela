#pragma once

#include <cstdint>

namespace vela::reflection {

inline constexpr std::uint8_t kRecordSlots = 6;

// 编译器与运行时共享操作编码；显式保留既有编号，记录字段占用连续槽位。
enum class Operation : std::uint8_t {
  TypeOf = 0,
  GetTypes = 1,
  FindType = 2,
  Revision = 3,
  Id = 4,
  Name = 5,
  Module = 6,
  Kind = 7,
  IsNullable = 8,
  IsRegistered = 9,
  TypeArguments = 10,
  BaseType = 11,
  Interfaces = 12,
  Signature = 13,
  IsInstance = 14,
  IsAssignableFrom = 15,
  GetFields = 16,
  FindField = 17,
  GetProperties = 18,
  FindProperty = 19,
  GetMethods = 20,
  FindMethod = 21,
  GetConstructor = 22,
  GetEnumMembers = 23,
  FindEnumMember = 24,
  DeclaringType = 25,
  Visibility = 26,
  IsStatic = 27,
  ValueType = 28,
  CanRead = 29,
  CanWrite = 30,
  Getter = 31,
  Setter = 32,
  GetValue = 33,
  SetValue = 34,
  Parameters = 35,
  ReturnType = 36,
  IsAsync = 37,
  Invoke = 38,
  Construct = 39,
  Ordinal = 40,
  EnumValue = 41,
  RecordItem = 42,
  FindAttributedMethods = RecordItem + kRecordSlots,
};

}  // namespace vela::reflection
