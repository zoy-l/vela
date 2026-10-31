#include "compiler/frontend/ast.h"
#include "compiler/frontend/type_checker.h"
#include "compiler/frontend/type_names.h"
#include "compiler/ir/ir.h"
#include "compiler/ir/lowerer.h"

#include <bit>
#include <cstdint>
#include <optional>
#include <string_view>

namespace vela::ir {

std::optional<Opcode> Lowerer::SelectDoubleOpcode(std::string_view operation) {
  if (operation == "+") {
    return Opcode::AddDouble;
  }
  if (operation == "-") {
    return Opcode::SubDouble;
  }
  if (operation == "*") {
    return Opcode::MulDouble;
  }
  if (operation == "/") {
    return Opcode::DivDouble;
  }
  if (operation == "==") {
    return Opcode::EqualDouble;
  }
  if (operation == "!=") {
    return Opcode::NotEqualDouble;
  }
  if (operation == "<") {
    return Opcode::LessDouble;
  }
  if (operation == "<=") {
    return Opcode::LessEqualDouble;
  }
  if (operation == ">") {
    return Opcode::GreaterDouble;
  }
  if (operation == ">=") {
    return Opcode::GreaterEqualDouble;
  }
  return std::nullopt;
}

Lowerer::LoweredValue Lowerer::LowerDoubleLiteral(const frontend::Expression& expression) {
  const auto bits = std::bit_cast<std::uint64_t>(expression.double_value);
  const auto next = static_cast<std::uint32_t>(current_function_->doubles.size());
  const auto [entry, inserted] = double_constant_ids_.try_emplace(bits, next);
  if (inserted) {
    current_function_->doubles.push_back(expression.double_value);
  }
  const RegisterId reg = AllocateTemporary(expression.location);
  Emit({
      .opcode = Opcode::LoadDouble,
      .dst = reg,
      .index = entry->second,
      .location = expression.location,
  });
  return {
      .reg = reg,
      .type = {.kind = frontend::TypeDescriptor::Kind::Double, .name = frontend::kDoubleTypeName},
  };
}

}  // namespace vela::ir
