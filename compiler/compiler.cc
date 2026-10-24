#include "compiler/compiler.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "base/builtin.h"
#include "compiler/frontend/ast.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/parser.h"
#include "compiler/frontend/source_loader.h"
#include "compiler/frontend/type_checker.h"
#include "compiler/ir/ir.h"
#include "compiler/ir/lowerer.h"
#include "compiler/ir/optimizer.h"
#include "compiler/register_allocator.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/object.h"
#include "runtime/value.h"
#include "vm/bytecode.h"
#include "vm/program.h"

namespace vela::compiler {
namespace {

void AddDiagnostic(frontend::Diagnostics& diagnostics,
                   frontend::SourceLocation location,
                   std::string message) {
  diagnostics.push_back({.location = location, .message = std::move(message), .source_path = {}});
}

auto FitsImmediate16(std::int64_t value) -> bool {
  return value >= std::numeric_limits<std::int16_t>::min() &&
         value <= std::numeric_limits<std::int16_t>::max();
}

auto Byte(std::uint32_t value) -> std::uint8_t {
  return static_cast<std::uint8_t>(value);
}

struct EncodingContext final {
  const ir::Function* function = nullptr;
  std::size_t code_size = 0;
  std::size_t function_count = 0;
  std::size_t class_count = 0;
  std::span<const std::string> source_paths;
};

auto EncodeInstruction(const ir::Instruction& instruction, const EncodingContext& context)
    -> std::expected<vm::Instruction, frontend::Diagnostic> {
  const auto& function = *context.function;
  const auto code_size = context.code_size;
  const auto function_count = context.function_count;
  const auto class_count = context.class_count;
  const std::string_view source_path =
      context.source_paths.empty()
          ? std::string_view()
          : context.source_paths.subspan(instruction.location.source_id).front();
  const bool uses_wide_target =
      instruction.opcode == ir::Opcode::Jump || instruction.opcode == ir::Opcode::JumpIfFalse;

  if (instruction.dst > std::numeric_limits<std::uint8_t>::max() ||
      instruction.lhs > std::numeric_limits<std::uint8_t>::max() ||
      instruction.rhs > std::numeric_limits<std::uint8_t>::max() ||
      (!uses_wide_target && instruction.index > std::numeric_limits<std::uint8_t>::max())) {
    return std::unexpected(frontend::Diagnostic{
        .location = instruction.location,
        .message = "instruction operand exceeds VM encoding",
        .source_path = std::string(source_path),
    });
  }
  const vm::OpCode opcode = instruction.opcode;
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
            .source_path = std::string(source_path),
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
    case ir::Opcode::EqualValue:
    case ir::Opcode::NotEqualValue:
    case ir::Opcode::LessInt:
    case ir::Opcode::LessEqualInt:
    case ir::Opcode::GreaterInt:
    case ir::Opcode::GreaterEqualInt:
    case ir::Opcode::AndBool:
    case ir::Opcode::OrBool:
    case ir::Opcode::StringConcat:
    case ir::Opcode::StringInterpolate:
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
            .source_path = std::string(source_path),
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
            .source_path = std::string(source_path),
        });
      }
      return vm::EncodeABC(opcode, Byte(instruction.dst),
                           static_cast<std::uint8_t>(instruction.index), 0);
    case ir::Opcode::Print:
      return vm::EncodeABC(opcode, 0, Byte(instruction.lhs), 0);
    case ir::Opcode::BuiltinCall:
      if (instruction.index >= static_cast<std::uint32_t>(Builtin::Count)) {
        return std::unexpected(frontend::Diagnostic{
            .location = instruction.location,
            .message = "invalid builtin ID in IR",
            .source_path = std::string(source_path),
        });
      }
      return vm::EncodeABC(opcode, Byte(instruction.dst),
                           static_cast<std::uint8_t>(instruction.index), Byte(instruction.lhs));
    case ir::Opcode::Call:
      if (instruction.index >= function_count) {
        return std::unexpected(frontend::Diagnostic{
            .location = instruction.location,
            .message = "unknown function ID in IR",
            .source_path = std::string(source_path),
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
            .source_path = std::string(source_path),
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
      if (instruction.index >= function.instructions.size()) {
        return std::unexpected(frontend::Diagnostic{
            .location = instruction.location,
            .message = "invalid jump target in IR",
            .source_path = std::string(source_path),
        });
      }
      const std::int64_t offset =
          static_cast<std::int64_t>(instruction.index) - static_cast<std::int64_t>(code_size + 1);
      if (!FitsImmediate16(offset)) {
        return std::unexpected(frontend::Diagnostic{
            .location = instruction.location,
            .message = "jump target exceeds bootstrap bytecode range",
            .source_path = std::string(source_path),
        });
      }
      const std::uint8_t condition =
          instruction.opcode == ir::Opcode::JumpIfFalse ? Byte(instruction.lhs) : 0;
      return vm::EncodeAImm16(opcode, condition, static_cast<std::int16_t>(offset));
    }
  }

  return std::unexpected(frontend::Diagnostic{
      .location = instruction.location,
      .message = "invalid IR opcode",
      .source_path = std::string(source_path),
  });
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
    const auto element_name =
        element.nullable ? element.name.substr(0, element.name.size() - 1) : element.name;
    runtime::ArrayLayout layout;
    layout.nullable_elements = element.nullable;
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
      case Kind::StringBuilder:
        layout.element_kind = runtime::ValueKind::Object;
        layout.element_object_kind = runtime::ObjectKind::StringBuilder;
        break;
      case Kind::Class:
        layout.element_kind = runtime::ValueKind::Object;
        layout.element_class = classes_by_name.at(element_name);
        break;
      case Kind::Array:
        layout.element_kind = runtime::ValueKind::Object;
        layout.element_array = arrays_by_name.at(element_name);
        break;
      case Kind::Invalid:
      case Kind::Null:
      case Kind::Void:
        return std::unexpected(frontend::Diagnostic{
            .location = frontend::SourceLocation(),
            .message = "invalid array element type",
            .source_path = {},
        });
    }
    const auto* stored = isolate.CreateArrayLayout(layout);
    arrays_by_name.emplace(type.name, stored);
    layouts.push_back(stored);
  }
  return layouts;
}

auto OptimizeAndAllocate(ir::Program& program, std::span<const std::string> source_paths)
    -> std::optional<frontend::Diagnostic> {
  for (auto& function : program.functions) {
    ir::Optimize(function, program.functions);
    auto failure = AllocateRegisters(function, program.functions);
    if (failure) {
      if (!source_paths.empty()) {
        failure->source_path = source_paths.subspan(failure->location.source_id).front();
      }
      return failure;
    }
  }
  return std::nullopt;
}

auto CompileParsed(frontend::ParseResult parsed, runtime::Isolate& isolate) -> CompileResult {
  if (!parsed.diagnostics.empty()) {
    return {.program = {}, .diagnostics = std::move(parsed.diagnostics)};
  }
  auto checked = frontend::TypeChecker().Check(parsed.program);
  if (!checked.diagnostics.empty()) {
    return {.program = {}, .diagnostics = std::move(checked.diagnostics)};
  }
  return CompileProgram(parsed.program, checked.model, isolate);
}

}  // namespace

auto CompileSource(std::string_view source, runtime::Isolate& isolate) -> CompileResult {
  return CompileParsed(frontend::ParseSource(source), isolate);
}

auto CompileSources(const std::vector<std::string_view>& sources, runtime::Isolate& isolate)
    -> CompileResult {
  frontend::ParseResult combined;
  for (const auto source : sources) {
    const auto source_id = static_cast<std::uint32_t>(combined.program.source_paths.size());
    combined.program.source_paths.push_back("<source:" + std::to_string(source_id + 1) + ">");
    auto parsed = frontend::ParseSource(source, source_id);
    if (!parsed.diagnostics.empty()) {
      for (auto& diagnostic : parsed.diagnostics) {
        diagnostic.source_path = combined.program.source_paths.back();
      }
      combined.diagnostics.insert(combined.diagnostics.end(),
                                  std::make_move_iterator(parsed.diagnostics.begin()),
                                  std::make_move_iterator(parsed.diagnostics.end()));
    } else {
      frontend::AppendProgram(combined.program, std::move(parsed.program));
    }
  }
  return CompileParsed(std::move(combined), isolate);
}

auto CompileFile(std::string_view path, runtime::Isolate& isolate) -> CompileResult {
  return CompileParsed(frontend::LoadProgram(path), isolate);
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

  if (auto failure = OptimizeAndAllocate(lowered.program, source.source_paths)) {
    return {.program = {}, .diagnostics = {std::move(*failure)}};
  }

  CompileResult result;
  std::vector<runtime::ClassLayout*> class_layouts;
  class_layouts.reserve(lowered.program.classes.size());
  for (const ir::Class& class_info : lowered.program.classes) {
    std::vector<std::uint32_t> reference_fields;
    for (std::size_t index = 0; index < class_info.field_types.size(); ++index) {
      const auto kind = class_info.field_types.at(index).kind;
      if (kind == frontend::TypeDescriptor::Kind::Class ||
          kind == frontend::TypeDescriptor::Kind::Array ||
          kind == frontend::TypeDescriptor::Kind::String ||
          kind == frontend::TypeDescriptor::Kind::StringBuilder) {
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

  std::vector<vm::FunctionStorage> functions;
  functions.reserve(lowered.program.functions.size());
  std::shared_ptr<vm::StringConstants> strings;
  const auto& literals = lowered.program.strings;
  if (!literals.empty()) {
    strings = std::make_shared<vm::StringConstants>(isolate.heap(), literals.size());
    for (std::size_t index = 0; index < literals.size(); ++index) {
      auto* string = isolate.heap().AllocateString(literals.at(index));
      if (string == nullptr) {
        AddDiagnostic(result.diagnostics, frontend::SourceLocation(),
                      "out of memory allocating string constant");
        return result;
      }
      strings->values.at(index) = string;
    }
  }
  for (const ir::Function& function : lowered.program.functions) {
    vm::FunctionStorage compiled;
    compiled.name = function.name;
    compiled.classes = class_layouts;
    compiled.strings = strings;
    compiled.arrays = *array_layouts;
    compiled.doubles = function.doubles;
    compiled.register_count = static_cast<std::uint16_t>(function.register_count);
    compiled.parameter_count = static_cast<std::uint16_t>(function.parameter_count);
    compiled.code.reserve(function.instructions.size());
    for (const ir::Instruction& instruction : function.instructions) {
      auto encoded =
          EncodeInstruction(instruction, EncodingContext{
                                             .function = &function,
                                             .code_size = compiled.code.size(),
                                             .function_count = lowered.program.functions.size(),
                                             .class_count = class_layouts.size(),
                                             .source_paths = source.source_paths,
                                         });
      if (!encoded) {
        result.diagnostics.push_back(std::move(encoded.error()));
        return result;
      }
      compiled.code.push_back(*encoded);
    }
    functions.push_back(std::move(compiled));
  }
  auto prepared = vm::Program::Create(std::move(functions), isolate);
  if (!prepared) {
    const auto& error = prepared.error();
    const auto location =
        lowered.program.functions.at(error.function).instructions.at(error.instruction).location;
    AddDiagnostic(result.diagnostics, location, error.message);
    return result;
  }
  result.program = std::move(*prepared);
  return result;
}

}  // namespace vela::compiler
