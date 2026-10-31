#pragma once

#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "base/callable.h"
#include "base/member.h"
#include "base/opcode.h"
#include "runtime/native/native_function.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"
#include "vm/debug_info.h"
#include "vm/exception_handler.h"

namespace vela::vm {

using Instruction = std::uint32_t;

inline constexpr unsigned kOperandAShift = 8;
inline constexpr unsigned kOperandBShift = 16;
inline constexpr unsigned kOperandCShift = 24;
inline constexpr Instruction kByteMask = std::numeric_limits<std::uint8_t>::max();
inline constexpr Instruction kImmediateMask = std::numeric_limits<std::uint16_t>::max();
inline constexpr unsigned kIndexBits = std::numeric_limits<std::uint8_t>::digits;
inline constexpr unsigned kConstantIndexBits = std::numeric_limits<std::uint16_t>::digits;

using OpCode = ::vela::Opcode;

// 前缀只扩展池索引，不改变寄存器宽度；其后的操作码仍占一个 32 位字。
inline constexpr Instruction kIndexPrefix = kByteMask;

[[nodiscard]] constexpr bool IsIndexPrefix(Instruction instruction) noexcept {
  return (instruction & kByteMask) == kIndexPrefix;
}

[[nodiscard]] constexpr Instruction EncodeIndexPrefix(std::uint32_t index) noexcept {
  return (index & ~kByteMask) | kIndexPrefix;
}

[[nodiscard]] constexpr unsigned IndexOperandBits(OpCode opcode) noexcept {
  switch (opcode) {
    case OpCode::LoadString:
    case OpCode::LoadDouble:
    case OpCode::LoadIntConstant:
      return kConstantIndexBits;
    case OpCode::NewArray:
    case OpCode::NewDynamicArray:
    case OpCode::NewCollection:
    case OpCode::Call:
    case OpCode::CallInterface:
    case OpCode::GetGlobal:
    case OpCode::SetGlobal:
    case OpCode::IsType:
    case OpCode::CheckedCast:
    case OpCode::NewInstance:
    case OpCode::SetField:
    case OpCode::GetField:
      return kIndexBits;
    default:
      return 0;
  }
}

[[nodiscard]] constexpr Instruction EncodeABC(OpCode opcode,
                                              std::uint8_t operand_a,
                                              std::uint8_t operand_b,
                                              std::uint8_t operand_c) noexcept {
  return static_cast<Instruction>(opcode) |
         (static_cast<Instruction>(operand_a) << kOperandAShift) |
         (static_cast<Instruction>(operand_b) << kOperandBShift) |
         (static_cast<Instruction>(operand_c) << kOperandCShift);
}

[[nodiscard]] constexpr Instruction EncodeAImm16(OpCode opcode,
                                                 std::uint8_t operand_a,
                                                 std::int16_t immediate) noexcept {
  return static_cast<Instruction>(opcode) |
         (static_cast<Instruction>(operand_a) << kOperandAShift) |
         (static_cast<Instruction>(static_cast<std::uint16_t>(immediate)) << kOperandBShift);
}

[[nodiscard]] constexpr OpCode DecodeOp(Instruction instruction) noexcept {
  return static_cast<OpCode>(instruction & kByteMask);
}

[[nodiscard]] constexpr std::uint8_t DecodeA(Instruction instruction) noexcept {
  return static_cast<std::uint8_t>((instruction >> kOperandAShift) & kByteMask);
}

[[nodiscard]] constexpr std::uint8_t DecodeB(Instruction instruction) noexcept {
  return static_cast<std::uint8_t>((instruction >> kOperandBShift) & kByteMask);
}

[[nodiscard]] constexpr std::uint8_t DecodeC(Instruction instruction) noexcept {
  return static_cast<std::uint8_t>((instruction >> kOperandCShift) & kByteMask);
}

[[nodiscard]] constexpr std::int16_t DecodeImm16(Instruction instruction) noexcept {
  return static_cast<std::int16_t>((instruction >> kOperandBShift) & kImmediateMask);
}

[[nodiscard]] constexpr Instruction EncodeAUnsigned16(OpCode opcode,
                                                      std::uint8_t operand_a,
                                                      std::uint16_t index) noexcept {
  return static_cast<Instruction>(opcode) |
         (static_cast<Instruction>(operand_a) << kOperandAShift) |
         (static_cast<Instruction>(index) << kOperandBShift);
}

[[nodiscard]] constexpr std::uint16_t DecodeUnsigned16(Instruction instruction) noexcept {
  return static_cast<std::uint16_t>((instruction >> kOperandBShift) & kImmediateMask);
}

[[nodiscard]] constexpr std::uint32_t DecodeIndex(Instruction instruction) noexcept {
  const auto opcode = DecodeOp(instruction);
  if (IndexOperandBits(opcode) == kConstantIndexBits) {
    return DecodeUnsigned16(instruction);
  }
  if (opcode == OpCode::GetField || opcode == OpCode::IsType || opcode == OpCode::CheckedCast) {
    return DecodeC(instruction);
  }
  return DecodeB(instruction);
}

// code 与元数据均为借用视图；字符串常量须由所有者持续保根。
struct Function final {
  std::span<const Instruction> code;
  std::span<runtime::ClassLayout* const> classes;
  std::span<runtime::StringObject* const> strings;
  std::span<const runtime::ArrayLayout* const> arrays;
  std::span<const double> doubles;
  std::uint16_t register_count = 0;
  std::uint16_t parameter_count = 0;
  bool is_async = false;
  std::span<const runtime::TypeConstraint> types;
  std::span<const runtime::CollectionLayout* const> collections;
  DebugInfo debug;
  const runtime::NativeFunction* native = nullptr;
  std::span<const ExceptionHandler> handlers;
  std::span<const std::int64_t> integers;
  const CallSignature* signature = nullptr;
  const std::vector<MemberDefinition>* members = nullptr;
};

}  // namespace vela::vm
