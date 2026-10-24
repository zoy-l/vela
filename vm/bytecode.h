#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "runtime/object.h"

namespace vela::vm {

using Instruction = std::uint32_t;

enum class OpCode : std::uint8_t {
  LoadNull,
  LoadBool,
  LoadInt,
  Move,
  AddInt,
  NewInstance,
  SetField,
  GetField,
  Return,
};

[[nodiscard]] constexpr Instruction EncodeABC(OpCode opcode,
                                              std::uint8_t a,
                                              std::uint8_t b,
                                              std::uint8_t c) noexcept {
  return static_cast<Instruction>(opcode) | (static_cast<Instruction>(a) << 8) |
         (static_cast<Instruction>(b) << 16) | (static_cast<Instruction>(c) << 24);
}

[[nodiscard]] constexpr Instruction EncodeAImm16(OpCode opcode,
                                                 std::uint8_t a,
                                                 std::int16_t immediate) noexcept {
  return static_cast<Instruction>(opcode) | (static_cast<Instruction>(a) << 8) |
         (static_cast<Instruction>(static_cast<std::uint16_t>(immediate)) << 16);
}

[[nodiscard]] constexpr OpCode DecodeOp(Instruction instruction) noexcept {
  return static_cast<OpCode>(instruction & 0xffu);
}

[[nodiscard]] constexpr std::uint8_t DecodeA(Instruction instruction) noexcept {
  return static_cast<std::uint8_t>((instruction >> 8) & 0xffu);
}

[[nodiscard]] constexpr std::uint8_t DecodeB(Instruction instruction) noexcept {
  return static_cast<std::uint8_t>((instruction >> 16) & 0xffu);
}

[[nodiscard]] constexpr std::uint8_t DecodeC(Instruction instruction) noexcept {
  return static_cast<std::uint8_t>((instruction >> 24) & 0xffu);
}

[[nodiscard]] constexpr std::int16_t DecodeImm16(Instruction instruction) noexcept {
  return static_cast<std::int16_t>((instruction >> 16) & 0xffffu);
}

struct Function final {
  std::span<const Instruction> code;
  std::span<runtime::ClassLayout* const> classes;
  std::uint8_t register_count = 0;
};

}  // namespace vela::vm
