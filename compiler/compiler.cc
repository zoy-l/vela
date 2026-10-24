#include "compiler/compiler.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include "compiler/frontend/ast.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/lexer.h"
#include "compiler/frontend/parser.h"
#include "compiler/frontend/type_checker.h"
#include "compiler/ir/ir.h"
#include "compiler/ir/lowerer.h"
#include "runtime/isolate.h"
#include "runtime/object.h"
#include "runtime/value.h"
#include "vm/bytecode.h"

namespace vela::compiler {
namespace {

void AddDiagnostic(frontend::Diagnostics& diagnostics,
                   frontend::SourceLocation location,
                   std::string message) {
  diagnostics.push_back({.location = location, .message = std::move(message)});
}

auto FitsImmediate16(std::int64_t value) -> bool {
  return value >= std::numeric_limits<std::int16_t>::min() &&
         value <= std::numeric_limits<std::int16_t>::max();
}

void FoldConstants(ir::Function& function) {
  for (std::size_t index = 2; index < function.instructions.size(); ++index) {
    ir::Instruction& instruction = function.instructions.at(index);
    const ir::Instruction& left = function.instructions.at(index - 2);
    const ir::Instruction& right = function.instructions.at(index - 1);
    if (left.opcode != ir::Opcode::LoadInt || right.opcode != ir::Opcode::LoadInt ||
        left.dst != instruction.lhs || right.dst != instruction.rhs) {
      continue;
    }
    std::int64_t value = 0;
    bool folded = true;
    switch (instruction.opcode) {
      case ir::Opcode::AddInt:
        folded = !__builtin_add_overflow(left.immediate, right.immediate, &value);
        break;
      case ir::Opcode::SubInt:
        folded = !__builtin_sub_overflow(left.immediate, right.immediate, &value);
        break;
      case ir::Opcode::MulInt:
        folded = !__builtin_mul_overflow(left.immediate, right.immediate, &value);
        break;
      case ir::Opcode::DivInt:
        folded =
            right.immediate != 0 &&
            (left.immediate != std::numeric_limits<std::int64_t>::min() || right.immediate != -1);
        if (folded) {
          value = left.immediate / right.immediate;
        }
        break;
      case ir::Opcode::LessInt:
        value = static_cast<std::int64_t>(left.immediate < right.immediate);
        break;
      case ir::Opcode::LessEqualInt:
        value = static_cast<std::int64_t>(left.immediate <= right.immediate);
        break;
      case ir::Opcode::GreaterInt:
        value = static_cast<std::int64_t>(left.immediate > right.immediate);
        break;
      case ir::Opcode::GreaterEqualInt:
        value = static_cast<std::int64_t>(left.immediate >= right.immediate);
        break;
      default:
        folded = false;
        break;
    }
    if (folded) {
      instruction.opcode = (instruction.opcode == ir::Opcode::LessInt ||
                            instruction.opcode == ir::Opcode::LessEqualInt ||
                            instruction.opcode == ir::Opcode::GreaterInt ||
                            instruction.opcode == ir::Opcode::GreaterEqualInt)
                               ? ir::Opcode::LoadBool
                               : ir::Opcode::LoadInt;
      instruction.immediate = value;
      instruction.lhs = 0;
      instruction.rhs = 0;
    }
  }
}

auto Byte(std::uint16_t value) -> std::uint8_t {
  return static_cast<std::uint8_t>(value);
}

auto EmitOpcode(ir::Opcode opcode) -> vm::OpCode {
  switch (opcode) {
    case ir::Opcode::LoadNull:
      return vm::OpCode::LoadNull;
    case ir::Opcode::LoadBool:
      return vm::OpCode::LoadBool;
    case ir::Opcode::LoadInt:
      return vm::OpCode::LoadInt;
    case ir::Opcode::LoadDouble:
      return vm::OpCode::LoadDouble;
    case ir::Opcode::AddDouble:
      return vm::OpCode::AddDouble;
    case ir::Opcode::SubDouble:
      return vm::OpCode::SubDouble;
    case ir::Opcode::MulDouble:
      return vm::OpCode::MulDouble;
    case ir::Opcode::DivDouble:
      return vm::OpCode::DivDouble;
    case ir::Opcode::NegDouble:
      return vm::OpCode::NegDouble;
    case ir::Opcode::EqualDouble:
      return vm::OpCode::EqualDouble;
    case ir::Opcode::NotEqualDouble:
      return vm::OpCode::NotEqualDouble;
    case ir::Opcode::LessDouble:
      return vm::OpCode::LessDouble;
    case ir::Opcode::LessEqualDouble:
      return vm::OpCode::LessEqualDouble;
    case ir::Opcode::GreaterDouble:
      return vm::OpCode::GreaterDouble;
    case ir::Opcode::GreaterEqualDouble:
      return vm::OpCode::GreaterEqualDouble;
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
    case ir::Opcode::LessInt:
      return vm::OpCode::LessInt;
    case ir::Opcode::LessEqualInt:
      return vm::OpCode::LessEqualInt;
    case ir::Opcode::GreaterInt:
      return vm::OpCode::GreaterInt;
    case ir::Opcode::GreaterEqualInt:
      return vm::OpCode::GreaterEqualInt;
    case ir::Opcode::AndBool:
      return vm::OpCode::AndBool;
    case ir::Opcode::OrBool:
      return vm::OpCode::OrBool;
    case ir::Opcode::StringConcat:
      return vm::OpCode::StringConcat;
    case ir::Opcode::EqualString:
      return vm::OpCode::EqualString;
    case ir::Opcode::NotEqualString:
      return vm::OpCode::NotEqualString;
    case ir::Opcode::LoadString:
      return vm::OpCode::LoadString;
    case ir::Opcode::NewArray:
      return vm::OpCode::NewArray;
    case ir::Opcode::GetIndex:
      return vm::OpCode::GetIndex;
    case ir::Opcode::SetIndex:
      return vm::OpCode::SetIndex;
    case ir::Opcode::Print:
      return vm::OpCode::Print;
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

struct EncodingContext final {
  const ir::Function* function;
  std::size_t code_size;
  std::size_t function_count;
  std::size_t class_count;
};

auto EncodeInstruction(const ir::Instruction& instruction, const EncodingContext& context)
    -> std::expected<vm::Instruction, frontend::Diagnostic> {
  const auto& function = *context.function;
  const auto code_size = context.code_size;
  const auto function_count = context.function_count;
  const auto class_count = context.class_count;
  const bool uses_wide_target =
      instruction.opcode == ir::Opcode::Jump || instruction.opcode == ir::Opcode::JumpIfFalse;

  if (instruction.dst > std::numeric_limits<std::uint8_t>::max() ||
      instruction.lhs > std::numeric_limits<std::uint8_t>::max() ||
      instruction.rhs > std::numeric_limits<std::uint8_t>::max() ||
      (!uses_wide_target && instruction.index > std::numeric_limits<std::uint8_t>::max())) {
    return std::unexpected(frontend::Diagnostic{
        .location = instruction.location,
        .message = "instruction operand exceeds VM encoding",
    });
  }
  const vm::OpCode opcode = EmitOpcode(instruction.opcode);
  switch (instruction.opcode) {
    case ir::Opcode::LoadNull:
      return vm::EncodeABC(opcode, Byte(instruction.dst), 0, 0);
    case ir::Opcode::LoadBool:
      return vm::EncodeABC(opcode, Byte(instruction.dst),
                           static_cast<std::uint8_t>(instruction.immediate != 0), 0);
    case ir::Opcode::LoadInt:
      if (!FitsImmediate16(instruction.immediate)) {
        return std::unexpected(frontend::Diagnostic{
            .location = instruction.location,
            .message = "integer literal exceeds bootstrap bytecode immediate range",
        });
      }
      return vm::EncodeAImm16(opcode, Byte(instruction.dst),
                              static_cast<std::int16_t>(instruction.immediate));
    case ir::Opcode::Move:
    case ir::Opcode::AddInt:
    case ir::Opcode::AddDouble:
    case ir::Opcode::SubDouble:
    case ir::Opcode::MulDouble:
    case ir::Opcode::DivDouble:
    case ir::Opcode::EqualDouble:
    case ir::Opcode::NotEqualDouble:
    case ir::Opcode::LessDouble:
    case ir::Opcode::LessEqualDouble:
    case ir::Opcode::GreaterDouble:
    case ir::Opcode::GreaterEqualDouble:
    case ir::Opcode::SubInt:
    case ir::Opcode::MulInt:
    case ir::Opcode::DivInt:
    case ir::Opcode::EqualInt:
    case ir::Opcode::NotEqualInt:
    case ir::Opcode::EqualBool:
    case ir::Opcode::NotEqualBool:
    case ir::Opcode::LessInt:
    case ir::Opcode::LessEqualInt:
    case ir::Opcode::GreaterInt:
    case ir::Opcode::GreaterEqualInt:
    case ir::Opcode::AndBool:
    case ir::Opcode::OrBool:
    case ir::Opcode::StringConcat:
    case ir::Opcode::EqualString:
    case ir::Opcode::NotEqualString:
    case ir::Opcode::GetIndex:
    case ir::Opcode::SetIndex:
      return vm::EncodeABC(opcode, Byte(instruction.dst), Byte(instruction.lhs),
                           Byte(instruction.rhs));
    case ir::Opcode::NewArray:
      if (instruction.index > std::numeric_limits<std::uint8_t>::max() ||
          instruction.immediate < 0 ||
          std::cmp_greater(instruction.immediate, std::numeric_limits<std::uint8_t>::max())) {
        return std::unexpected(frontend::Diagnostic{
            .location = instruction.location,
            .message = "array layout index or length exceeds bytecode encoding",
        });
      }
      return vm::EncodeABC(opcode, Byte(instruction.dst),
                           static_cast<std::uint8_t>(instruction.index),
                           static_cast<std::uint8_t>(instruction.immediate));
    case ir::Opcode::LoadString:
    case ir::Opcode::LoadDouble:
      if (instruction.index > std::numeric_limits<std::uint8_t>::max()) {
        return std::unexpected(frontend::Diagnostic{
            .location = instruction.location,
            .message = "constant index exceeds bytecode encoding",
        });
      }
      return vm::EncodeABC(opcode, Byte(instruction.dst),
                           static_cast<std::uint8_t>(instruction.index), 0);
    case ir::Opcode::Print:
      return vm::EncodeABC(opcode, 0, Byte(instruction.lhs), 0);
    case ir::Opcode::Call:
      if (instruction.index >= function_count) {
        return std::unexpected(frontend::Diagnostic{
            .location = instruction.location,
            .message = "unknown function ID in IR",
        });
      }
      return vm::EncodeABC(opcode, Byte(instruction.dst),
                           static_cast<std::uint8_t>(instruction.index), Byte(instruction.lhs));
    case ir::Opcode::NegInt:
    case ir::Opcode::NotBool:
    case ir::Opcode::NegDouble:
      return vm::EncodeABC(opcode, Byte(instruction.dst), Byte(instruction.lhs), 0);
    case ir::Opcode::NewInstance:
      if (instruction.index >= class_count) {
        return std::unexpected(frontend::Diagnostic{
            .location = instruction.location,
            .message = "unknown class ID in IR",
        });
      }
      return vm::EncodeABC(opcode, Byte(instruction.dst),
                           static_cast<std::uint8_t>(instruction.index), 0);
    case ir::Opcode::SetField:
      return vm::EncodeABC(opcode, Byte(instruction.dst),
                           static_cast<std::uint8_t>(instruction.index), Byte(instruction.lhs));
    case ir::Opcode::GetField:
      return vm::EncodeABC(opcode, Byte(instruction.dst),
                           static_cast<std::uint8_t>(instruction.lhs),
                           static_cast<std::uint8_t>(instruction.index));
    case ir::Opcode::Return:
      return vm::EncodeABC(opcode, Byte(instruction.dst), 0, 0);
    case ir::Opcode::Jump:
    case ir::Opcode::JumpIfFalse: {
      if (instruction.index > function.instructions.size()) {
        return std::unexpected(frontend::Diagnostic{
            .location = instruction.location,
            .message = "invalid jump target in IR",
        });
      }
      const std::int64_t offset =
          static_cast<std::int64_t>(instruction.index) - static_cast<std::int64_t>(code_size + 1);
      if (!FitsImmediate16(offset)) {
        return std::unexpected(frontend::Diagnostic{
            .location = instruction.location,
            .message = "jump target exceeds bootstrap bytecode range",
        });
      }
      const std::uint8_t condition =
          instruction.opcode == ir::Opcode::JumpIfFalse ? Byte(instruction.lhs) : 0;
      return vm::EncodeAImm16(opcode, condition, static_cast<std::int16_t>(offset));
    }
  }

  return std::unexpected(
      frontend::Diagnostic{.location = instruction.location, .message = "invalid IR opcode"});
}

auto CreateArrayLayouts(const ir::Program& program,
                        const frontend::SemanticModel& model,
                        std::span<runtime::ClassLayout* const> class_layouts,
                        runtime::Isolate& isolate)
    -> std::expected<std::vector<const runtime::ArrayLayout*>, frontend::Diagnostic> {
  if (program.array_types.empty()) {
    return std::vector<const runtime::ArrayLayout*>();
  }
  absl::flat_hash_map<std::string, runtime::ClassLayout*> classes_by_name;
  classes_by_name.reserve(program.classes.size());
  for (std::size_t index = 0; index < program.classes.size(); ++index) {
    classes_by_name.emplace(program.classes.at(index).name, class_layouts.subspan(index).front());
  }
  std::vector<const runtime::ArrayLayout*> layouts;
  layouts.reserve(program.array_types.size());
  absl::flat_hash_map<std::string, const runtime::ArrayLayout*> arrays_by_name;
  arrays_by_name.reserve(program.array_types.size());
  for (const auto& type : program.array_types) {
    const auto& element = model.types.at(type.name.substr(0, type.name.size() - 2));
    runtime::ArrayLayout layout;
    using Kind = frontend::TypeDescriptor::Kind;
    switch (element.kind) {
      case Kind::Bool:
        layout.element_kind = runtime::ValueKind::Boolean;
        break;
      case Kind::Int:
      case Kind::Enum:
        layout.element_kind = runtime::ValueKind::Integer;
        break;
      case Kind::Double:
        layout.element_kind = runtime::ValueKind::Double;
        break;
      case Kind::String:
        layout.element_kind = runtime::ValueKind::String;
        break;
      case Kind::Class:
        layout.element_kind = runtime::ValueKind::Object;
        layout.element_class = classes_by_name.at(element.name);
        break;
      case Kind::Array:
        layout.element_kind = runtime::ValueKind::Object;
        layout.element_array = arrays_by_name.at(element.name);
        break;
      case Kind::Invalid:
      case Kind::Void:
        return std::unexpected(frontend::Diagnostic{
            .location = frontend::SourceLocation(),
            .message = "invalid array element type",
        });
    }
    const auto* stored = isolate.CreateArrayLayout(layout);
    arrays_by_name.emplace(type.name, stored);
    layouts.push_back(stored);
  }
  return layouts;
}

}  // namespace

auto CompiledProgram::FindFunction(std::string_view name) const noexcept
    -> const CompiledFunction* {
  const auto found = std::ranges::find_if(
      functions, [name](const auto& function) -> auto { return function.name == name; });
  return found == functions.end() ? nullptr : &*found;
}

auto CompileSource(std::string_view source, runtime::Isolate& isolate) -> CompileResult {
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

auto CompileSources(const std::vector<std::string_view>& sources, runtime::Isolate& isolate)
    -> CompileResult {
  frontend::Program program;
  frontend::Diagnostics diagnostics;
  for (const std::string_view source : sources) {
    frontend::LexResult lexed = frontend::Lexer(source).Tokenize();
    diagnostics.insert(diagnostics.end(), std::make_move_iterator(lexed.diagnostics.begin()),
                       std::make_move_iterator(lexed.diagnostics.end()));
    if (!lexed.diagnostics.empty()) {
      continue;
    }
    frontend::ParseResult parsed = frontend::Parser(std::move(lexed.tokens)).Parse();
    diagnostics.insert(diagnostics.end(), std::make_move_iterator(parsed.diagnostics.begin()),
                       std::make_move_iterator(parsed.diagnostics.end()));
    if (!parsed.diagnostics.empty()) {
      continue;
    }
    for (auto& item : parsed.program.enums) {
      program.enums.push_back(std::move(item));
    }
    for (auto& item : parsed.program.classes) {
      program.classes.push_back(std::move(item));
    }
    for (auto& item : parsed.program.functions) {
      program.functions.push_back(std::move(item));
    }
    for (auto& item : parsed.program.imports) {
      program.imports.push_back(std::move(item));
    }
  }
  if (!diagnostics.empty()) {
    return CompileResult{.program = CompiledProgram(), .diagnostics = std::move(diagnostics)};
  }
  frontend::TypeCheckResult checked = frontend::TypeChecker().Check(program);
  if (!checked.diagnostics.empty()) {
    return CompileResult{
        .program = CompiledProgram(),
        .diagnostics = std::move(checked.diagnostics),
    };
  }
  return CompileProgram(program, checked.model, isolate);
}

auto CompileFile(std::string_view path, runtime::Isolate& isolate) -> CompileResult {
  std::vector<std::string> contents;
  absl::flat_hash_set<std::string> visiting;
  absl::flat_hash_set<std::string> loaded;
  frontend::Diagnostics diagnostics;
  const auto load = [&](const auto& self, const std::filesystem::path& file) -> void {
    const std::string key = std::filesystem::weakly_canonical(file).string();
    if (!visiting.insert(key).second) {
      diagnostics.push_back({
          .location = frontend::SourceLocation(),
          .message = "cyclic import involving '" + key + "'",
      });
      return;
    }
    if (loaded.contains(key)) {
      visiting.erase(key);
      return;
    }
    std::ifstream input(key, std::ios::binary);
    if (!input) {
      diagnostics.push_back({
          .location = frontend::SourceLocation(),
          .message = "cannot open imported source '" + key + "'",
      });
      visiting.erase(key);
      return;
    }
    std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    frontend::LexResult lexed = frontend::Lexer(source).Tokenize();
    frontend::ParseResult parsed = frontend::Parser(std::move(lexed.tokens)).Parse();
    diagnostics.insert(diagnostics.end(), std::make_move_iterator(lexed.diagnostics.begin()),
                       std::make_move_iterator(lexed.diagnostics.end()));
    diagnostics.insert(diagnostics.end(), std::make_move_iterator(parsed.diagnostics.begin()),
                       std::make_move_iterator(parsed.diagnostics.end()));
    if (lexed.diagnostics.empty() && parsed.diagnostics.empty()) {
      for (const std::string& import : parsed.program.imports) {
        self(self, std::filesystem::path(key).parent_path() / import);
      }
    }
    contents.push_back(std::move(source));
    loaded.insert(key);
    visiting.erase(key);
  };
  load(load, std::filesystem::path(path));
  if (!diagnostics.empty()) {
    return CompileResult{.program = CompiledProgram(), .diagnostics = std::move(diagnostics)};
  }
  std::vector<std::string_view> views;
  views.reserve(contents.size());
  for (const std::string& source : contents) {
    views.push_back(source);
  }
  return CompileSources(views, isolate);
}

auto CompileProgram(const frontend::Program& source,
                    const frontend::SemanticModel& model,
                    runtime::Isolate& isolate) -> CompileResult {
  ir::LoweringResult lowered = ir::Lowerer(source, model).Lower();
  if (!lowered.Succeeded()) {
    CompileResult result;
    result.diagnostics = std::move(lowered.diagnostics);
    return result;
  }
  for (ir::Function& function : lowered.program.functions) {
    FoldConstants(function);
  }

  CompileResult result;
  std::vector<runtime::ClassLayout*> class_layouts;
  class_layouts.reserve(lowered.program.classes.size());
  for (const ir::Class& class_info : lowered.program.classes) {
    std::vector<std::uint32_t> reference_fields;
    for (std::size_t index = 0; index < class_info.field_types.size(); ++index) {
      const auto kind = class_info.field_types.at(index).kind;
      if (kind == frontend::TypeDescriptor::Kind::Class ||
          kind == frontend::TypeDescriptor::Kind::Array) {
        reference_fields.push_back(static_cast<std::uint32_t>(index));
      }
    }
    runtime::ClassLayout* layout = isolate.CreateClassLayout(
        static_cast<std::uint32_t>(class_info.field_types.size()), reference_fields);
    class_layouts.push_back(layout);
  }

  const auto array_layouts = CreateArrayLayouts(lowered.program, model, class_layouts, isolate);
  if (!array_layouts) {
    result.diagnostics.push_back(array_layouts.error());
    return result;
  }

  result.program.functions.reserve(lowered.program.functions.size());
  result.program.strings.reserve(lowered.program.strings.size());
  for (const std::string& value : lowered.program.strings) {
    result.program.strings.push_back(isolate.CreateString(value));
  }
  for (const ir::Function& function : lowered.program.functions) {
    if (function.register_count > std::numeric_limits<std::uint8_t>::max()) {
      AddDiagnostic(result.diagnostics, frontend::SourceLocation(),
                    "function register count exceeds VM encoding");
      return result;
    }
    CompiledFunction compiled;
    compiled.name = function.name;
    compiled.classes = class_layouts;
    compiled.strings = result.program.strings;
    compiled.arrays = *array_layouts;
    compiled.doubles = function.doubles;
    // 没有操作数的无限循环在 IR 中可使用零个寄存器；VM 帧仍保留一个槽位。
    compiled.register_count =
        static_cast<std::uint8_t>(std::max(function.register_count, std::uint16_t{1}));
    compiled.parameter_count = function.parameter_count;
    compiled.code.reserve(function.instructions.size());
    for (const ir::Instruction& instruction : function.instructions) {
      auto encoded =
          EncodeInstruction(instruction, EncodingContext{
                                             .function = &function,
                                             .code_size = compiled.code.size(),
                                             .function_count = lowered.program.functions.size(),
                                             .class_count = class_layouts.size(),
                                         });
      if (!encoded) {
        result.diagnostics.push_back(std::move(encoded.error()));
        return result;
      }
      compiled.code.push_back(*encoded);
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
