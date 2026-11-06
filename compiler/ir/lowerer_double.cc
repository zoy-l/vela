#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/syntax/token.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/frontend/typing/type_names.h"
#include "compiler/ir/ir.h"
#include "compiler/ir/lowerer.h"

#include <bit>
#include <cstdint>
#include <utility>

namespace vela::ir {

using frontend::TokenKind;

Opcode Lowerer::SelectDoubleOpcode(TokenKind operation) {
  if (operation == TokenKind::Plus) {
    return Opcode::AddDouble;
  }
  if (operation == TokenKind::Minus) {
    return Opcode::SubDouble;
  }
  if (operation == TokenKind::Star) {
    return Opcode::MulDouble;
  }
  if (operation == TokenKind::Slash) {
    return Opcode::DivDouble;
  }
  if (operation == TokenKind::EqualEqual) {
    return Opcode::EqualDouble;
  }
  if (operation == TokenKind::BangEqual) {
    return Opcode::NotEqualDouble;
  }
  if (operation == TokenKind::Less) {
    return Opcode::LessDouble;
  }
  if (operation == TokenKind::LessEqual) {
    return Opcode::LessEqualDouble;
  }
  if (operation == TokenKind::Greater) {
    return Opcode::GreaterDouble;
  }
  if (operation == TokenKind::GreaterEqual) {
    return Opcode::GreaterEqualDouble;
  }
  std::unreachable();
}

Lowerer::LoweredValue Lowerer::EmitDoubleConstant(double value, frontend::SourceLocation location) {
  const auto bits = std::bit_cast<std::uint64_t>(value);
  const auto next = static_cast<std::uint32_t>(current_function_->doubles.size());
  const auto [entry, inserted] = double_constant_ids_.try_emplace(bits, next);
  if (inserted) {
    current_function_->doubles.push_back(value);
  }
  const RegisterId reg = AllocateRegister(location);
  Emit({
      .opcode = Opcode::LoadDouble,
      .dst = reg,
      .index = entry->second,
      .location = location,
  });
  return {
      .reg = reg,
      .type = {.kind = frontend::TypeDescriptor::Kind::Double, .name = frontend::kDoubleTypeName},
  };
}

}  // namespace vela::ir
