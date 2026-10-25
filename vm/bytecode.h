#pragma once

#include <cstdint>
#include <limits>
#include <span>

#include "base/opcode.h"
#include "runtime/native_function.h"
#include "runtime/object.h"
#include "runtime/value.h"
#include "vm/debug_info.h"
#include "vm/exception_handler.h"

namespace vela::vm {

using Instruction = std::uint32_t;

inline constexpr unsigned kOperandAShift = 8;
inline constexpr unsigned kOperandBShift = 16;
inline constexpr unsigned kOperandCShift = 24;
inline constexpr Instruction kByteMask = std::numeric_limits<std::uint8_t>::max();
inline constexpr Instruction kImmediateMask = std::numeric_limits<std::uint16_t>::max();

using OpCode = ::vela::Opcode;

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

// code 与元数据均为借用视图；字符串常量须由所有者持续保根。
struct Function final {
  std::span<const Instruction> code;
  std::span<runtime::ClassLayout* const> classes;
  std::span<runtime::StringObject* const> strings;
  std::span<const runtime::ArrayLayout* const> arrays;
  std::span<const double> doubles;
  std::uint16_t register_count = 0;
  std::uint16_t parameter_count = 0;
  std::span<const runtime::TypeConstraint> types;
  std::span<const runtime::CollectionLayout* const> collections;
  DebugInfo debug;
  const runtime::NativeFunction* native = nullptr;
  std::span<const ExceptionHandler> handlers;
  std::span<const std::int64_t> integers;
};

}  // namespace vela::vm
