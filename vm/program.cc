#include "vm/program.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "base/builtin.h"
#include "base/execution_limits.h"
#include "runtime/isolate.h"
#include "runtime/string.h"
#include "vm/bytecode.h"

namespace vela::vm {
namespace {

auto IsRegister(std::uint8_t index, const Function& function) noexcept -> bool {
  return index < function.register_count;
}

auto IsValidInstruction(Instruction instruction,
                        const Function& function,
                        std::span<const FunctionStorage> functions) noexcept -> bool {
  const OpCode opcode = DecodeOp(instruction);
  const auto operand_a = DecodeA(instruction);
  const auto operand_b = DecodeB(instruction);
  const auto operand_c = DecodeC(instruction);
  switch (opcode) {
    case OpCode::LoadNull:
    case OpCode::LoadBool:
    case OpCode::LoadInt:
    case OpCode::Return:
      return IsRegister(operand_a, function);
    case OpCode::NewArray:
      return IsRegister(operand_a, function) && operand_b < function.arrays.size() &&
             function.arrays.subspan(operand_b).front() != nullptr;
    case OpCode::LoadDouble:
      return IsRegister(operand_a, function) && operand_b < function.doubles.size();
    case OpCode::Move:
    case OpCode::NegInt:
    case OpCode::NegDouble:
    case OpCode::NotBool:
    case OpCode::GetField:
      return IsRegister(operand_a, function) && IsRegister(operand_b, function);
    case OpCode::AddInt:
    case OpCode::AddDouble:
    case OpCode::SubDouble:
    case OpCode::MulDouble:
    case OpCode::DivDouble:
    case OpCode::EqualDouble:
    case OpCode::NotEqualDouble:
    case OpCode::LessDouble:
    case OpCode::LessEqualDouble:
    case OpCode::GreaterDouble:
    case OpCode::GreaterEqualDouble:
    case OpCode::SubInt:
    case OpCode::MulInt:
    case OpCode::DivInt:
    case OpCode::EqualInt:
    case OpCode::NotEqualInt:
    case OpCode::EqualBool:
    case OpCode::NotEqualBool:
    case OpCode::EqualValue:
    case OpCode::NotEqualValue:
    case OpCode::LessInt:
    case OpCode::LessEqualInt:
    case OpCode::GreaterInt:
    case OpCode::GreaterEqualInt:
    case OpCode::AndBool:
    case OpCode::OrBool:
    case OpCode::StringConcat:
    case OpCode::EqualString:
    case OpCode::NotEqualString:
    case OpCode::GetIndex:
    case OpCode::SetIndex:
      return IsRegister(operand_a, function) && IsRegister(operand_b, function) &&
             IsRegister(operand_c, function);
    case OpCode::Call:
      return IsRegister(operand_a, function) && operand_b < functions.size() &&
             static_cast<std::size_t>(operand_c) +
                     functions.subspan(operand_b).front().parameter_count <=
                 function.register_count;
    case OpCode::BuiltinCall:
      return IsRegister(operand_a, function) &&
             operand_b < static_cast<std::uint8_t>(Builtin::Count) &&
             static_cast<std::size_t>(operand_c) + BuiltinArity(static_cast<Builtin>(operand_b)) <=
                 function.register_count;
    case OpCode::StringInterpolate:
      return IsRegister(operand_a, function) &&
             static_cast<std::size_t>(operand_b) + operand_c <= function.register_count;
    case OpCode::Jump:
      return true;
    case OpCode::JumpIfFalse:
      return IsRegister(operand_a, function);
    case OpCode::NewInstance:
      return IsRegister(operand_a, function) && operand_b < function.classes.size() &&
             function.classes.subspan(operand_b).front() != nullptr;
    case OpCode::LoadString:
      return IsRegister(operand_a, function) && operand_b < function.strings.size() &&
             function.strings.subspan(operand_b).front() != nullptr;
    case OpCode::Print:
      return IsRegister(operand_b, function);
    case OpCode::SetField:
      return IsRegister(operand_a, function) && IsRegister(operand_c, function);
  }
  return false;
}

auto ValidateFunctions(std::span<const FunctionStorage> storage)
    -> std::expected<void, ValidationError> {
  for (std::size_t index = 0; index < storage.size(); ++index) {
    const auto function = storage.subspan(index).front().View();
    if (function.code.empty() || function.register_count == 0 ||
        function.register_count > kMaxFrameRegisters ||
        function.parameter_count > function.register_count) {
      return std::unexpected(ValidationError{
          .function = index,
          .instruction = 0,
          .message = "invalid function metadata",
      });
    }
    for (std::size_t position = 0; position < function.code.size(); ++position) {
      const auto instruction = function.code.subspan(position).front();
      if (!IsValidInstruction(instruction, function, storage)) {
        return std::unexpected(ValidationError{
            .function = index,
            .instruction = position,
            .message = "invalid instruction operand",
        });
      }
      const auto opcode = DecodeOp(instruction);
      if (opcode == OpCode::Jump || opcode == OpCode::JumpIfFalse) {
        const auto target = static_cast<std::int64_t>(position) + 1 + DecodeImm16(instruction);
        if (target < 0 || static_cast<std::uint64_t>(target) >= function.code.size()) {
          return std::unexpected(ValidationError{
              .function = index,
              .instruction = position,
              .message = "jump target is outside the function",
          });
        }
      }
    }
    const auto last = DecodeOp(function.code.back());
    if (last != OpCode::Return && last != OpCode::Jump) {
      return std::unexpected(ValidationError{
          .function = index,
          .instruction = function.code.size() - 1,
          .message = "function can fall off its bytecode",
      });
    }
  }
  return {};
}

}  // namespace

auto FunctionStorage::View() const noexcept -> Function {
  return {
      .code = code,
      .classes = classes,
      .strings = strings != nullptr ? std::span<runtime::StringObject* const>(strings->values)
                                    : std::span<runtime::StringObject* const>(),
      .arrays = arrays,
      .doubles = doubles,
      .register_count = register_count,
      .parameter_count = parameter_count,
  };
}

auto Program::Create(std::vector<FunctionStorage> functions, const runtime::Isolate& isolate)
    -> std::expected<Program, ValidationError> {
  const auto valid = ValidateFunctions(functions);
  if (!valid) {
    return std::unexpected(valid.error());
  }
  return Program(std::make_shared<const ProgramStorage>(
      ProgramStorage{.functions = std::move(functions), .isolate = &isolate}));
}

auto Program::Load(std::span<const Function> functions, runtime::Isolate& isolate)
    -> std::expected<Program, ValidationError> {
  std::vector<FunctionStorage> copied;
  copied.reserve(functions.size());
  for (const auto& function : functions) {
    FunctionStorage stored;
    stored.code.assign(function.code.begin(), function.code.end());
    stored.classes.assign(function.classes.begin(), function.classes.end());
    stored.arrays.assign(function.arrays.begin(), function.arrays.end());
    stored.doubles.assign(function.doubles.begin(), function.doubles.end());
    stored.register_count = function.register_count;
    stored.parameter_count = function.parameter_count;
    if (!function.strings.empty()) {
      auto strings = std::make_shared<StringConstants>(isolate.heap(), function.strings);
      stored.strings = std::move(strings);
    }
    copied.push_back(std::move(stored));
  }
  return Create(std::move(copied), isolate);
}

auto Program::functions() const noexcept -> std::span<const FunctionStorage> {
  return storage_ != nullptr ? std::span<const FunctionStorage>(storage_->functions)
                             : std::span<const FunctionStorage>();
}

auto Program::FindFunction(std::string_view name) const noexcept -> FunctionHandle {
  const auto stored = functions();
  const auto found = std::ranges::find_if(
      stored, [name](const auto& function) -> bool { return function.name == name; });
  if (found == stored.end()) {
    return {};
  }
  return {*this, static_cast<std::uint32_t>(found - stored.begin())};
}

auto FunctionHandle::IsValid() const noexcept -> bool {
  return program_.storage_ != nullptr;
}

auto Program::IsForIsolate(const runtime::Isolate& isolate) const noexcept -> bool {
  return storage_ != nullptr && storage_->isolate == &isolate;
}

auto FunctionHandle::parameter_count() const noexcept -> std::uint16_t {
  return program_.functions().subspan(index_).front().parameter_count;
}

auto FunctionHandle::register_count() const noexcept -> std::uint16_t {
  return program_.functions().subspan(index_).front().register_count;
}

auto FunctionHandle::code() const noexcept -> std::span<const Instruction> {
  const auto& function = program_.storage_->functions.at(index_);
  return function.code;
}

auto FunctionHandle::doubles() const noexcept -> std::span<const double> {
  const auto& function = program_.storage_->functions.at(index_);
  return function.doubles;
}

}  // namespace vela::vm
