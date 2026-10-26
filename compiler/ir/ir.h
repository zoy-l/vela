#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/callable.h"
#include "base/opcode.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/type_checker.h"

namespace vela::ir {

using RegisterId = std::uint32_t;
using ClassId = std::uint32_t;
using FieldId = std::uint32_t;

using ::vela::Opcode;

struct Instruction final {
  Opcode opcode = Opcode::LoadNull;
  RegisterId dst = 0;
  RegisterId lhs = 0;
  RegisterId rhs = 0;
  std::int64_t immediate = 0;
  std::uint32_t index = 0;
  frontend::SourceLocation location;
  std::optional<std::uint32_t> handler = std::nullopt;
};

struct Class final {
  enum class Kind : std::uint8_t { Instance, Expected, RuntimeError, DateParts };
  std::string name;
  std::vector<std::string> field_order;
  std::vector<frontend::TypeDescriptor> field_types;
  std::optional<CallableTarget> callable;
  std::vector<std::uint32_t> interfaces;
  std::vector<std::uint32_t> dispatch_targets;
  Kind kind = Kind::Instance;
};

struct Function final {
  struct Handler final {
    std::uint32_t target = 0;
  };
  std::string name;
  std::string owner_class;
  frontend::TypeDescriptor return_type;
  std::uint32_t parameter_count = 0;
  std::uint32_t register_count = 0;
  std::vector<Instruction> instructions;
  std::vector<double> doubles;
  std::string native_symbol;
  std::vector<Handler> handlers;
  bool is_async = false;
};

struct Program final {
  std::vector<Class> classes;
  std::vector<Function> functions;
  std::vector<std::string> strings;
  std::vector<frontend::TypeDescriptor> array_types;
  std::vector<std::uint32_t> global_initializers;
  std::vector<frontend::TypeDescriptor> runtime_types;
  std::vector<frontend::TypeDescriptor> collection_types;
};

}  // namespace vela::ir
