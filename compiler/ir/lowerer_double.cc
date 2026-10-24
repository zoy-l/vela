#include "compiler/ir/lowerer.h"

#include <bit>
#include <optional>

namespace vela::ir {
namespace {

auto SelectDoubleOpcode(std::string_view operation) -> std::optional<Opcode> {
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

}  // namespace

auto Lowerer::LowerDoubleLiteral(const frontend::Expression& expression) -> LoweredValue {
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
  return {.reg = reg, .type = {.kind = frontend::TypeDescriptor::Kind::Double, .name = "double"}};
}

auto Lowerer::LowerDoubleBinary(const frontend::Expression& expression,
                                const LoweredValue& left,
                                const LoweredValue& right) -> LoweredValue {
  const auto opcode = SelectDoubleOpcode(expression.text);
  if (!opcode) {
    AddDiagnostic(expression.location, "unsupported double operation");
    return {};
  }
  const RegisterId result = AllocateTemporary(expression.location);
  Emit({
      .opcode = *opcode,
      .dst = result,
      .lhs = left.reg,
      .rhs = right.reg,
      .location = expression.location,
  });
  const bool comparison = expression.text == "==" || expression.text == "!=" ||
                          expression.text == "<" || expression.text == "<=" ||
                          expression.text == ">" || expression.text == ">=";
  return {
      .reg = result,
      .type = comparison ? frontend::TypeDescriptor{.kind = frontend::TypeDescriptor::Kind::Bool,
                                                    .name = "bool",
                        }
                         : left.type,
  };
}

}  // namespace vela::ir
