#include "compiler/compiler.h"
#include "compiler/compilation_state.h"
#include "runtime/layout_transaction.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "base/async_operation.h"
#include "base/builtin.h"
#include "compiler/frontend/ast.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/parser.h"
#include "compiler/frontend/source_loader.h"
#include "compiler/frontend/specialization.h"
#include "compiler/frontend/type_checker.h"
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"
#include "compiler/ir/lowerer.h"
#include "compiler/ir/optimizer.h"
#include "compiler/native_linker.h"
#include "compiler/register_allocator.h"
#include "compiler/runtime_layouts.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/object.h"
#include "vm/bytecode.h"
#include "vm/program.h"

namespace vela::compiler {
namespace {

void AddDiagnostic(frontend::Diagnostics& diagnostics,
                   frontend::SourceLocation location,
                   std::string message) {
  diagnostics.push_back({.location = location, .message = std::move(message), .source_path = {}});
}

bool FitsImmediate16(std::int64_t value) {
  return value >= std::numeric_limits<std::int16_t>::min() &&
         value <= std::numeric_limits<std::int16_t>::max();
}

std::uint8_t Byte(std::uint32_t value) {
  return static_cast<std::uint8_t>(value);
}

struct EncodingContext final {
  const ir::Function* function = nullptr;
  std::size_t code_size = 0;
  std::size_t function_count = 0;
  std::size_t class_count = 0;
  std::span<const std::string> source_paths;
};

std::optional<frontend::Diagnostic> ValidateEncodingOperands(const ir::Instruction& instruction,
                                                             std::string_view source_path) {
  const bool uses_wide_target = instruction.opcode == ir::Opcode::Jump ||
                                instruction.opcode == ir::Opcode::JumpIfFalse ||
                                instruction.opcode == ir::Opcode::JumpIfNull;
  const bool uses_wide_constant = instruction.opcode == ir::Opcode::LoadString ||
                                  instruction.opcode == ir::Opcode::LoadDouble ||
                                  instruction.opcode == ir::Opcode::LoadIntConstant;
  const std::uint32_t index_limit = uses_wide_constant ? std::numeric_limits<std::uint16_t>::max()
                                                       : std::numeric_limits<std::uint8_t>::max();

  if (instruction.dst > std::numeric_limits<std::uint8_t>::max() ||
      instruction.lhs > std::numeric_limits<std::uint8_t>::max() ||
      instruction.rhs > std::numeric_limits<std::uint8_t>::max() ||
      (!uses_wide_target && instruction.index > index_limit)) {
    return frontend::Diagnostic{
        .location = instruction.location,
        .message = "instruction operand exceeds VM encoding",
        .source_path = std::string(source_path),
    };
  }
  return std::nullopt;
}

std::expected<vm::Instruction, frontend::Diagnostic> EncodeJump(const ir::Instruction& instruction,
                                                                const EncodingContext& context,
                                                                std::string_view source_path) {
  if (instruction.index >= context.function->instructions.size()) {
    return std::unexpected(frontend::Diagnostic{
        .location = instruction.location,
        .message = "invalid jump target in IR",
        .source_path = std::string(source_path),
    });
  }
  const std::int64_t offset = static_cast<std::int64_t>(instruction.index) -
                              static_cast<std::int64_t>(context.code_size + 1);
  if (!FitsImmediate16(offset)) {
    return std::unexpected(frontend::Diagnostic{
        .location = instruction.location,
        .message = "jump target exceeds bootstrap bytecode range",
        .source_path = std::string(source_path),
    });
  }
  const std::uint8_t condition =
      instruction.opcode == ir::Opcode::JumpIfFalse || instruction.opcode == ir::Opcode::JumpIfNull
          ? Byte(instruction.lhs)
          : 0;
  return vm::EncodeAImm16(instruction.opcode, condition, static_cast<std::int16_t>(offset));
}

std::expected<vm::Instruction, frontend::Diagnostic> EncodeInstruction(
    const ir::Instruction& instruction,
    const EncodingContext& context) {
  const auto function_count = context.function_count;
  const auto class_count = context.class_count;
  const std::string_view source_path =
      context.source_paths.empty()
          ? std::string_view()
          : context.source_paths.subspan(instruction.location.source_id).front();
  if (auto diagnostic = ValidateEncodingOperands(instruction, source_path)) {
    return std::unexpected(std::move(*diagnostic));
  }
  const vm::OpCode opcode = instruction.opcode;
  switch (instruction.opcode) {
    case ir::Opcode::LoadNull:
    case ir::Opcode::Catch:
      return vm::EncodeABC(opcode, Byte(instruction.dst), 0, 0);
    case ir::Opcode::LoadBool:
      return vm::EncodeABC(opcode, Byte(instruction.dst),
                           static_cast<std::uint8_t>(instruction.immediate != 0), 0);
    case ir::Opcode::LoadInt:
      return vm::EncodeAImm16(opcode, Byte(instruction.dst),
                              static_cast<std::int16_t>(instruction.immediate));
    case ir::Opcode::Move:
    case ir::Opcode::ExceptionValue:
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
    case ir::Opcode::ModInt:
    case ir::Opcode::BitAndInt:
    case ir::Opcode::BitOrInt:
    case ir::Opcode::BitXorInt:
    case ir::Opcode::ShiftLeftInt:
    case ir::Opcode::ShiftRightInt:
    case ir::Opcode::ShiftUnsignedRightInt:
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
    case ir::Opcode::NewFuture:
    case ir::Opcode::SleepFuture:
    case ir::Opcode::StringInterpolate:
    case ir::Opcode::EqualString:
    case ir::Opcode::NotEqualString:
    case ir::Opcode::GetIndex:
    case ir::Opcode::SetIndex:
      return vm::EncodeABC(opcode, Byte(instruction.dst), Byte(instruction.lhs),
                           Byte(instruction.rhs));
    case ir::Opcode::NewArray:
      if (instruction.immediate < 0 ||
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
    case ir::Opcode::NewDynamicArray:
    case ir::Opcode::NewCollection:
      return vm::EncodeABC(opcode, Byte(instruction.dst), Byte(instruction.index),
                           Byte(instruction.lhs));
    case ir::Opcode::LoadString:
    case ir::Opcode::LoadDouble:
    case ir::Opcode::LoadIntConstant:
      return vm::EncodeAUnsigned16(opcode, Byte(instruction.dst),
                                   static_cast<std::uint16_t>(instruction.index));
    case ir::Opcode::Print:
    case ir::Opcode::Throw:
    case ir::Opcode::Rethrow:
      return vm::EncodeABC(opcode, 0, Byte(instruction.lhs), 0);
    case ir::Opcode::AsyncCall:
      if (instruction.index >= static_cast<std::uint32_t>(AsyncOperation::Count)) {
        return std::unexpected(frontend::Diagnostic{
            .location = instruction.location,
            .message = "invalid async operation in IR",
            .source_path = std::string(source_path),
        });
      }
      return vm::EncodeABC(opcode, Byte(instruction.dst), Byte(instruction.index),
                           Byte(instruction.lhs));
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
    case ir::Opcode::CallIndirect:
      return vm::EncodeABC(opcode, Byte(instruction.dst), Byte(instruction.rhs),
                           Byte(instruction.lhs));
    case ir::Opcode::NativeCall:
      if (instruction.immediate < 0 ||
          std::cmp_greater(instruction.immediate, std::numeric_limits<std::uint8_t>::max())) {
        return std::unexpected(frontend::Diagnostic{
            .location = instruction.location,
            .message = "native parameter count exceeds bytecode encoding",
            .source_path = std::string(source_path),
        });
      }
      return vm::EncodeABC(opcode, Byte(instruction.dst), Byte(instruction.lhs),
                           Byte(static_cast<std::uint32_t>(instruction.immediate)));
    case ir::Opcode::CallInterface:
      return vm::EncodeABC(opcode, Byte(instruction.dst), Byte(instruction.index),
                           Byte(instruction.lhs));
    case ir::Opcode::GetGlobal:
      return vm::EncodeABC(opcode, Byte(instruction.dst), Byte(instruction.index), 0);
    case ir::Opcode::IsType:
    case ir::Opcode::CheckedCast:
    case ir::Opcode::ExpectedValue:
      return vm::EncodeABC(opcode, Byte(instruction.dst), Byte(instruction.lhs),
                           Byte(instruction.index));
    case ir::Opcode::ExpectedHasValue:
      return vm::EncodeABC(opcode, Byte(instruction.dst), Byte(instruction.lhs), 0);
    case ir::Opcode::NewOk:
    case ir::Opcode::NewErr:
      if (instruction.index >= class_count) {
        return std::unexpected(frontend::Diagnostic{
            .location = instruction.location,
            .message = "unknown Expected layout",
            .source_path = std::string(source_path),
        });
      }
      return vm::EncodeABC(opcode, Byte(instruction.dst), Byte(instruction.index),
                           Byte(instruction.lhs));
    case ir::Opcode::SetGlobal:
      return vm::EncodeABC(opcode, Byte(instruction.lhs), Byte(instruction.index), 0);
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
    case ir::Opcode::BitNotInt:
    case ir::Opcode::NotBool:
    case ir::Opcode::NegDouble:
    case ir::Opcode::Await:
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
    case ir::Opcode::JumpIfFalse:
    case ir::Opcode::JumpIfNull:
      return EncodeJump(instruction, context, source_path);
  }

  return std::unexpected(frontend::Diagnostic{
      .location = instruction.location,
      .message = "invalid IR opcode",
      .source_path = std::string(source_path),
  });
}

std::expected<vm::FunctionStorage, frontend::Diagnostic> EncodeFunction(
    const ir::Function& function,
    EncodingContext context,
    vm::FunctionStorage compiled) {
  compiled.code.reserve(function.instructions.size());
  absl::flat_hash_map<std::int64_t, std::uint32_t> integer_ids;
  for (auto instruction : function.instructions) {
    if (instruction.opcode == ir::Opcode::LoadInt && !FitsImmediate16(instruction.immediate)) {
      const auto next = static_cast<std::uint32_t>(compiled.integers.size());
      const auto [entry, inserted] = integer_ids.try_emplace(instruction.immediate, next);
      if (inserted) {
        compiled.integers.push_back(instruction.immediate);
      }
      instruction.opcode = ir::Opcode::LoadIntConstant;
      instruction.index = entry->second;
    }
    context.code_size = compiled.code.size();
    const auto encoded = EncodeInstruction(instruction, context);
    if (!encoded) {
      return std::unexpected(encoded.error());
    }
    if (!context.source_paths.empty() &&
        (compiled.source_mappings.empty() ||
         compiled.source_mappings.back().location != instruction.location)) {
      compiled.source_mappings.push_back({
          .instruction = static_cast<std::uint32_t>(compiled.code.size()),
          .location = instruction.location,
      });
    }
    const auto begin = static_cast<std::uint32_t>(compiled.code.size());
    compiled.code.push_back(*encoded);
    if (!instruction.handler || !ir::MayThrow(instruction.opcode)) {
      continue;
    }
    const auto target = function.handlers.at(*instruction.handler).target;
    if (!compiled.handlers.empty() && compiled.handlers.back().end == begin &&
        compiled.handlers.back().target == target) {
      ++compiled.handlers.back().end;
    } else {
      compiled.handlers.push_back({.begin = begin, .end = begin + 1, .target = target});
    }
  }
  return compiled;
}

std::optional<frontend::Diagnostic> OptimizeAndAllocate(ir::Program& program,
                                                        std::span<const std::string> source_paths) {
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

}  // namespace

CompileResult CompileParsed(frontend::ParseResult parsed, runtime::Isolate& isolate) {
  if (!parsed.diagnostics.empty()) {
    return {.program = {}, .diagnostics = std::move(parsed.diagnostics)};
  }
  auto specialized = frontend::SpecializeGenerics(parsed.program);
  if (!specialized.empty()) {
    return {.program = {}, .diagnostics = std::move(specialized)};
  }
  auto checked = frontend::TypeChecker().Check(parsed.program);
  if (!checked.diagnostics.empty()) {
    return {.program = {}, .diagnostics = std::move(checked.diagnostics)};
  }
  return CompileProgram(parsed.program, checked.model, isolate);
}

CompileResult CompileSource(std::string_view source, runtime::Isolate& isolate) {
  const frontend::SourceInput input{.path = "main.vela", .text = source};
  return CompileParsed(frontend::LoadSources(std::span(&input, 1), input.path), isolate);
}

CompileResult CompileModules(std::span<const frontend::SourceInput> sources,
                             std::string_view entry,
                             runtime::Isolate& isolate) {
  return CompileParsed(frontend::LoadSources(sources, entry), isolate);
}

CompileResult CompileFile(std::string_view path, runtime::Isolate& isolate) {
  return CompileParsed(frontend::LoadProgram(path), isolate);
}

CompileResult CompileProgram(const frontend::Program& source,
                             const frontend::SemanticModel& model,
                             runtime::Isolate& isolate,
                             CompilationState* state) {
  auto native_diagnostics = CheckNativeBindings(source, model, isolate);
  if (!native_diagnostics.empty()) {
    return {.program = {}, .diagnostics = std::move(native_diagnostics)};
  }
  ir::LoweringResult lowered =
      ir::Lowerer(source, model, state == nullptr ? nullptr : &state->ids).Lower();
  if (!lowered.Succeeded()) {
    CompileResult result;
    result.diagnostics = std::move(lowered.diagnostics);
    return result;
  }

  std::vector<std::string> paths;
  paths.reserve(source.sources.size());
  for (const auto& file : source.sources) {
    paths.push_back(file.path);
  }
  if (auto failure = OptimizeAndAllocate(lowered.program, paths)) {
    return {.program = {}, .diagnostics = {std::move(*failure)}};
  }

  CompileResult result;
  runtime::LayoutTransaction layout_transaction(isolate);
  LayoutCache local_cache;
  auto* cache = state == nullptr ? &local_cache : &state->layouts;
  if (cache->code_identity == nullptr) {
    cache->code_identity = isolate.CreateCodeIdentity();
  }
  const auto interfaces = CreateInterfaceTypes(model, isolate, cache);
  auto class_layouts = CreateClassLayouts(lowered.program, interfaces, isolate, cache);

  auto layouts =
      CreateRuntimeLayouts(lowered.program, model, class_layouts, interfaces, isolate, cache);

  std::vector<vm::FunctionStorage> functions;
  auto shared_layouts = std::make_shared<const vm::LayoutTables>(
      std::move(class_layouts), std::move(layouts.arrays), std::move(layouts.collections),
      std::move(layouts.types), std::move(lowered.program.members));
  functions.reserve(lowered.program.functions.size());
  const auto source_paths = std::make_shared<const std::vector<std::string>>(std::move(paths));
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
    if (!function.native_symbol.empty()) {
      compiled.native = isolate.FindNative(function.native_symbol);
    }
    compiled.layouts = shared_layouts;
    compiled.strings = strings;
    compiled.doubles = function.doubles;
    compiled.register_count = static_cast<std::uint16_t>(function.register_count);
    compiled.parameter_count = static_cast<std::uint16_t>(function.parameter_count);
    compiled.is_async = function.is_async;
    compiled.signature = function.signature;
    compiled.source_paths = source_paths;
    auto encoded = EncodeFunction(function,
                                  EncodingContext{
                                      .function = &function,
                                      .function_count = lowered.program.functions.size(),
                                      .class_count = shared_layouts->classes().size(),
                                      .source_paths = *source_paths,
                                  },
                                  std::move(compiled));
    if (!encoded) {
      result.diagnostics.push_back(std::move(encoded.error()));
      return result;
    }
    functions.push_back(std::move(*encoded));
  }
  auto prepared = vm::Program::Create(
      std::move(functions), isolate, std::move(lowered.program.global_initializers),
      state != nullptr ? state->globals : nullptr, cache->code_identity);
  if (!prepared) {
    const auto& error = prepared.error();
    const auto location =
        lowered.program.functions.at(error.function).instructions.at(error.instruction).location;
    AddDiagnostic(result.diagnostics, location, error.message);
    return result;
  }
  result.program = std::move(*prepared);
  if (state != nullptr) {
    state->globals = result.program.shared_globals();
  }
  layout_transaction.Commit();
  return result;
}

}  // namespace vela::compiler
