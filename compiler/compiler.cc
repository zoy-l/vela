#include "compiler/compiler.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "compiler/frontend/lexer.h"
#include "compiler/frontend/parser.h"
#include "compiler/frontend/type_checker.h"
#include "compiler/ir/lowerer.h"

namespace vela::compiler {
namespace {

void AddDiagnostic(frontend::Diagnostics& diagnostics,
                   frontend::SourceLocation location,
                   std::string message) {
  diagnostics.push_back({location, std::move(message)});
}

bool FitsImmediate16(std::int64_t value) {
  return value >= std::numeric_limits<std::int16_t>::min() &&
         value <= std::numeric_limits<std::int16_t>::max();
}

std::uint8_t Byte(std::uint16_t value) {
  return static_cast<std::uint8_t>(value);
}

vm::OpCode EmitOpcode(ir::Opcode opcode) {
  switch (opcode) {
    case ir::Opcode::LoadNull:
      return vm::OpCode::LoadNull;
    case ir::Opcode::LoadBool:
      return vm::OpCode::LoadBool;
    case ir::Opcode::LoadInt:
      return vm::OpCode::LoadInt;
    case ir::Opcode::Move:
      return vm::OpCode::Move;
    case ir::Opcode::AddInt:
      return vm::OpCode::AddInt;
    case ir::Opcode::SubInt:
      return vm::OpCode::SubInt;
    case ir::Opcode::MulInt:
      return vm::OpCode::MulInt;
    case ir::Opcode::DivInt:
      return vm::OpCode::DivInt;
    case ir::Opcode::NegInt:
      return vm::OpCode::NegInt;
    case ir::Opcode::NotBool:
      return vm::OpCode::NotBool;
    case ir::Opcode::EqualInt:
      return vm::OpCode::EqualInt;
    case ir::Opcode::NotEqualInt:
      return vm::OpCode::NotEqualInt;
    case ir::Opcode::EqualBool:
      return vm::OpCode::EqualBool;
    case ir::Opcode::NotEqualBool:
      return vm::OpCode::NotEqualBool;
    case ir::Opcode::Call:
      return vm::OpCode::Call;
    case ir::Opcode::Jump:
      return vm::OpCode::Jump;
    case ir::Opcode::JumpIfFalse:
      return vm::OpCode::JumpIfFalse;
    case ir::Opcode::NewInstance:
      return vm::OpCode::NewInstance;
    case ir::Opcode::SetField:
      return vm::OpCode::SetField;
    case ir::Opcode::GetField:
      return vm::OpCode::GetField;
    case ir::Opcode::Return:
      return vm::OpCode::Return;
  }
  return vm::OpCode::LoadNull;
}

}  // namespace

const CompiledFunction* CompiledProgram::FindFunction(std::string_view name) const noexcept {
  const auto found = std::find_if(functions.begin(), functions.end(),
                                  [name](const auto& function) { return function.name == name; });
  return found == functions.end() ? nullptr : &*found;
}

CompileResult CompileSource(std::string_view source, runtime::Isolate& isolate) {
  frontend::LexResult lexed = frontend::Lexer(source).Tokenize();
  if (!lexed.diagnostics.empty()) {
    CompileResult result;
    result.diagnostics = std::move(lexed.diagnostics);
    return result;
  }

  frontend::ParseResult parsed = frontend::Parser(std::move(lexed.tokens)).Parse();
  if (!parsed.diagnostics.empty()) {
    CompileResult result;
    result.diagnostics = std::move(parsed.diagnostics);
    return result;
  }

  frontend::TypeCheckResult checked = frontend::TypeChecker().Check(parsed.program);
  if (!checked.diagnostics.empty()) {
    CompileResult result;
    result.diagnostics = std::move(checked.diagnostics);
    return result;
  }
  return CompileProgram(parsed.program, checked.model, isolate);
}

CompileResult CompileProgram(const frontend::Program& source,
                             const frontend::SemanticModel& model,
                             runtime::Isolate& isolate) {
  ir::LoweringResult lowered = ir::Lowerer(source, model).Lower();
  if (!lowered.Succeeded()) {
    CompileResult result;
    result.diagnostics = std::move(lowered.diagnostics);
    return result;
  }

  CompileResult result;
  std::vector<runtime::ClassLayout*> class_layouts;
  class_layouts.reserve(lowered.program.classes.size());
  for (const ir::Class& class_info : lowered.program.classes) {
    std::vector<std::uint32_t> reference_fields;
    for (std::size_t index = 0; index < class_info.field_types.size(); ++index) {
      if (class_info.field_types[index].kind == frontend::TypeDescriptor::Kind::Class) {
        reference_fields.push_back(static_cast<std::uint32_t>(index));
      }
    }
    runtime::ClassLayout* layout = isolate.CreateClassLayout(
        static_cast<std::uint32_t>(class_info.field_types.size()), reference_fields);
    if (layout == nullptr) {
      AddDiagnostic(result.diagnostics, {}, "unable to allocate class layout");
      return result;
    }
    class_layouts.push_back(layout);
  }

  result.program.functions.reserve(lowered.program.functions.size());
  for (const ir::Function& function : lowered.program.functions) {
    if (function.register_count == 0 || function.register_count > 255) {
      AddDiagnostic(result.diagnostics, {}, "function register count exceeds VM encoding");
      return result;
    }
    CompiledFunction compiled;
    compiled.name = function.name;
    compiled.classes = class_layouts;
    compiled.register_count = static_cast<std::uint8_t>(function.register_count);
    compiled.parameter_count = function.parameter_count;
    compiled.code.reserve(function.instructions.size());
    for (const ir::Instruction& instruction : function.instructions) {
      if (instruction.dst > 255 || instruction.lhs > 255 || instruction.rhs > 255 ||
          instruction.index > 255) {
        AddDiagnostic(result.diagnostics, instruction.location,
                      "instruction operand exceeds VM encoding");
        return result;
      }
      const vm::OpCode opcode = EmitOpcode(instruction.opcode);
      switch (instruction.opcode) {
        case ir::Opcode::LoadNull:
          compiled.code.push_back(vm::EncodeABC(opcode, Byte(instruction.dst), 0, 0));
          break;
        case ir::Opcode::LoadBool:
          compiled.code.push_back(
              vm::EncodeABC(opcode, Byte(instruction.dst),
                            static_cast<std::uint8_t>(instruction.immediate != 0), 0));
          break;
        case ir::Opcode::LoadInt:
          if (!FitsImmediate16(instruction.immediate)) {
            AddDiagnostic(result.diagnostics, instruction.location,
                          "integer literal exceeds bootstrap bytecode immediate range");
            return result;
          }
          compiled.code.push_back(vm::EncodeAImm16(
              opcode, Byte(instruction.dst), static_cast<std::int16_t>(instruction.immediate)));
          break;
        case ir::Opcode::Move:
        case ir::Opcode::AddInt:
        case ir::Opcode::SubInt:
        case ir::Opcode::MulInt:
        case ir::Opcode::DivInt:
        case ir::Opcode::EqualInt:
        case ir::Opcode::NotEqualInt:
        case ir::Opcode::EqualBool:
        case ir::Opcode::NotEqualBool:
          compiled.code.push_back(vm::EncodeABC(opcode, Byte(instruction.dst),
                                                Byte(instruction.lhs), Byte(instruction.rhs)));
          break;
        case ir::Opcode::Call:
          if (instruction.index >= lowered.program.functions.size()) {
            AddDiagnostic(result.diagnostics, instruction.location, "unknown function ID in IR");
            return result;
          }
          compiled.code.push_back(vm::EncodeABC(opcode, Byte(instruction.dst),
                                                static_cast<std::uint8_t>(instruction.index),
                                                Byte(instruction.lhs)));
          break;
        case ir::Opcode::NegInt:
        case ir::Opcode::NotBool:
          compiled.code.push_back(
              vm::EncodeABC(opcode, Byte(instruction.dst), Byte(instruction.lhs), 0));
          break;
        case ir::Opcode::NewInstance:
          if (instruction.index >= class_layouts.size()) {
            AddDiagnostic(result.diagnostics, instruction.location, "unknown class ID in IR");
            return result;
          }
          compiled.code.push_back(vm::EncodeABC(opcode, Byte(instruction.dst),
                                                static_cast<std::uint8_t>(instruction.index), 0));
          break;
        case ir::Opcode::SetField:
          compiled.code.push_back(vm::EncodeABC(opcode, Byte(instruction.dst),
                                                static_cast<std::uint8_t>(instruction.index),
                                                Byte(instruction.lhs)));
          break;
        case ir::Opcode::GetField:
          compiled.code.push_back(vm::EncodeABC(opcode, Byte(instruction.dst),
                                                static_cast<std::uint8_t>(instruction.lhs),
                                                static_cast<std::uint8_t>(instruction.index)));
          break;
        case ir::Opcode::Return:
          compiled.code.push_back(vm::EncodeABC(opcode, Byte(instruction.dst), 0, 0));
          break;
        case ir::Opcode::Jump:
        case ir::Opcode::JumpIfFalse: {
          if (instruction.index > function.instructions.size()) {
            AddDiagnostic(result.diagnostics, instruction.location, "invalid jump target in IR");
            return result;
          }
          const std::int64_t offset = static_cast<std::int64_t>(instruction.index) -
                                      static_cast<std::int64_t>(compiled.code.size() + 1);
          if (!FitsImmediate16(offset)) {
            AddDiagnostic(result.diagnostics, instruction.location,
                          "jump target exceeds bootstrap bytecode range");
            return result;
          }
          const std::uint8_t condition =
              instruction.opcode == ir::Opcode::JumpIfFalse ? Byte(instruction.lhs) : 0;
          compiled.code.push_back(
              vm::EncodeAImm16(opcode, condition, static_cast<std::int16_t>(offset)));
          break;
        }
      }
    }
    result.program.functions.push_back(std::move(compiled));
  }
  result.program.views.reserve(result.program.functions.size());
  for (const CompiledFunction& function : result.program.functions) {
    result.program.views.push_back(function.View());
  }
  return result;
}

}  // namespace vela::compiler
