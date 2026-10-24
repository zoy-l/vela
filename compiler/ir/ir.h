#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "compiler/frontend/ast.h"
#include "compiler/frontend/type_checker.h"

namespace vela::ir {

using RegisterId = std::uint16_t;
using ClassId = std::uint32_t;
using FieldId = std::uint32_t;

enum class Opcode : std::uint8_t {
  LoadNull,
  LoadBool,
  LoadInt,
  Move,
  AddInt,
  SubInt,
  MulInt,
  DivInt,
  NegInt,
  NotBool,
  EqualInt,
  NotEqualInt,
  EqualBool,
  NotEqualBool,
  Call,
  Jump,
  JumpIfFalse,
  NewInstance,
  SetField,
  GetField,
  Return,
};

struct Instruction final {
  Opcode opcode = Opcode::LoadNull;
  RegisterId dst = 0;
  RegisterId lhs = 0;
  RegisterId rhs = 0;
  std::int64_t immediate = 0;
  std::uint32_t index = 0;
  frontend::SourceLocation location;
};

struct Class final {
  std::string name;
  std::vector<std::string> field_order;
  std::vector<frontend::TypeDescriptor> field_types;
};

struct Function final {
  std::string name;
  frontend::TypeDescriptor return_type;
  std::uint16_t parameter_count = 0;
  std::uint16_t register_count = 0;
  std::vector<Instruction> instructions;
};

struct Program final {
  std::vector<Class> classes;
  std::vector<Function> functions;
};

}  // namespace vela::ir
