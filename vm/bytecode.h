#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include "runtime/object.h"

namespace vela::vm {

using Instruction = std::uint32_t;

inline constexpr unsigned kOperandAShift = 8;
inline constexpr unsigned kOperandBShift = 16;
inline constexpr unsigned kOperandCShift = 24;
inline constexpr Instruction kByteMask = std::numeric_limits<std::uint8_t>::max();
inline constexpr Instruction kImmediateMask = std::numeric_limits<std::uint16_t>::max();

enum class OpCode : std::uint8_t {
  LoadNull,
  LoadBool,
  LoadInt,
  LoadDouble,
  Move,
  AddInt,
  SubInt,
  MulInt,
  DivInt,
  AddDouble,
  SubDouble,
  MulDouble,
  DivDouble,
  NegDouble,
  EqualDouble,
  NotEqualDouble,
  LessDouble,
  LessEqualDouble,
  GreaterDouble,
  GreaterEqualDouble,
  NegInt,
  NotBool,
  EqualInt,
  NotEqualInt,
  EqualBool,
  NotEqualBool,
  LessInt,
  LessEqualInt,
  GreaterInt,
  GreaterEqualInt,
  AndBool,
  OrBool,
  StringConcat,
  EqualString,
  NotEqualString,
  LoadString,
  NewArray,
  GetIndex,
  SetIndex,
  Print,
  Call,
  Jump,
  JumpIfFalse,
  NewInstance,
  SetField,
  GetField,
  Return,
};

[[nodiscard]] constexpr auto EncodeABC(OpCode opcode,
                                       std::uint8_t operand_a,
                                       std::uint8_t operand_b,
                                       std::uint8_t operand_c) noexcept -> Instruction {
  return static_cast<Instruction>(opcode) |
         (static_cast<Instruction>(operand_a) << kOperandAShift) |
         (static_cast<Instruction>(operand_b) << kOperandBShift) |
         (static_cast<Instruction>(operand_c) << kOperandCShift);
}

[[nodiscard]] constexpr auto EncodeAImm16(OpCode opcode,
                                          std::uint8_t operand_a,
                                          std::int16_t immediate) noexcept -> Instruction {
  return static_cast<Instruction>(opcode) |
         (static_cast<Instruction>(operand_a) << kOperandAShift) |
         (static_cast<Instruction>(static_cast<std::uint16_t>(immediate)) << kOperandBShift);
}

[[nodiscard]] constexpr auto DecodeOp(Instruction instruction) noexcept -> OpCode {
  return static_cast<OpCode>(instruction & kByteMask);
}

[[nodiscard]] constexpr auto DecodeA(Instruction instruction) noexcept -> std::uint8_t {
  return static_cast<std::uint8_t>((instruction >> kOperandAShift) & kByteMask);
}

[[nodiscard]] constexpr auto DecodeB(Instruction instruction) noexcept -> std::uint8_t {
  return static_cast<std::uint8_t>((instruction >> kOperandBShift) & kByteMask);
}

[[nodiscard]] constexpr auto DecodeC(Instruction instruction) noexcept -> std::uint8_t {
  return static_cast<std::uint8_t>((instruction >> kOperandCShift) & kByteMask);
}

[[nodiscard]] constexpr auto DecodeImm16(Instruction instruction) noexcept -> std::int16_t {
  return static_cast<std::int16_t>((instruction >> kOperandBShift) & kImmediateMask);
}

struct Function final {
  std::span<const Instruction> code;
  std::span<runtime::ClassLayout* const> classes;
  std::span<runtime::StringObject* const> strings;
  std::span<const runtime::ArrayLayout* const> arrays;
  std::span<const double> doubles;
  std::uint8_t register_count = 0;
  std::uint8_t parameter_count = 0;
};

struct Program final {
  std::span<const Function> functions;
};

}  // namespace vela::vm
