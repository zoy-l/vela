#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/type_checker.h"

namespace vela::ir {

using RegisterId = std::uint16_t;
using ClassId = std::uint32_t;
using FieldId = std::uint32_t;

enum class Opcode : std::uint8_t {
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
  StringInterpolate,
  BuiltinCall,
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
  std::string owner_class;
  frontend::TypeDescriptor return_type;
  std::uint16_t parameter_count = 0;
  std::uint16_t register_count = 0;
  std::vector<Instruction> instructions;
  std::vector<double> doubles;
};

struct Program final {
  std::vector<Class> classes;
  std::vector<Function> functions;
  std::vector<std::string> strings;
  std::vector<frontend::TypeDescriptor> array_types;
};

}  // namespace vela::ir
