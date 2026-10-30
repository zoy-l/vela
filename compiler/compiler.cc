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

#include "compiler/frontend/ast.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/parser.h"
#include "compiler/frontend/source_loader.h"
#include "compiler/frontend/type_checker.h"
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"
#include "compiler/ir/preparation.h"
#include "compiler/native_linker.h"
#include "compiler/register_allocator.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/object.h"
#include "runtime/program_layouts.h"
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
  std::size_t code_size = 0;
  std::span<const std::string> source_paths;
  std::span<const std::uint32_t> instruction_offsets;
};

std::optional<frontend::Diagnostic> ValidateEncodingOperands(const ir::Instruction& instruction,
                                                             std::string_view source_path) {
  const bool uses_wide_target = instruction.opcode == ir::Opcode::Jump ||
                                instruction.opcode == ir::Opcode::JumpIfFalse ||
                                instruction.opcode == ir::Opcode::JumpIfNull;
  if (instruction.dst > std::numeric_limits<std::uint8_t>::max() ||
      instruction.lhs > std::numeric_limits<std::uint8_t>::max() ||
      instruction.rhs > std::numeric_limits<std::uint8_t>::max() ||
      (!uses_wide_target && vm::IndexOperandBits(instruction.opcode) == 0 &&
       instruction.index > std::numeric_limits<std::uint8_t>::max())) {
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
  const std::int64_t offset =
      static_cast<std::int64_t>(context.instruction_offsets.subspan(instruction.index).front()) -
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
            .message = "array literal length exceeds bytecode encoding",
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
      return vm::EncodeABC(opcode, Byte(instruction.dst), Byte(instruction.index),
                           Byte(instruction.lhs));
    case ir::Opcode::BuiltinCall:
      return vm::EncodeABC(opcode, Byte(instruction.dst),
                           static_cast<std::uint8_t>(instruction.index), Byte(instruction.lhs));
    case ir::Opcode::CallIndirect:
      return vm::EncodeABC(opcode, Byte(instruction.dst), Byte(instruction.rhs),
                           Byte(instruction.lhs));
    case ir::Opcode::CallInterface:
      return vm::EncodeABC(opcode, Byte(instruction.dst), Byte(instruction.index),
                           Byte(instruction.lhs));
    case ir::Opcode::GetGlobal:
      return vm::EncodeABC(opcode, Byte(instruction.dst), Byte(instruction.index), 0);
    case ir::Opcode::IsType:
    case ir::Opcode::CheckedCast:
      return vm::EncodeABC(opcode, Byte(instruction.dst), Byte(instruction.lhs),
                           Byte(instruction.index));
    case ir::Opcode::SetGlobal:
      return vm::EncodeABC(opcode, Byte(instruction.lhs), Byte(instruction.index), 0);
    case ir::Opcode::Call:
      return vm::EncodeABC(opcode, Byte(instruction.dst),
                           static_cast<std::uint8_t>(instruction.index), Byte(instruction.lhs));
    case ir::Opcode::NegInt:
    case ir::Opcode::BitNotInt:
    case ir::Opcode::NotBool:
    case ir::Opcode::NegDouble:
    case ir::Opcode::Await:
      return vm::EncodeABC(opcode, Byte(instruction.dst), Byte(instruction.lhs), 0);
    case ir::Opcode::NewInstance:
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

  std::unreachable();
}

void BindIntegerConstants(std::span<ir::Instruction> instructions,
                          std::vector<std::int64_t>& integers) {
  absl::flat_hash_map<std::int64_t, std::uint32_t> integer_ids;
  for (auto& instruction : instructions) {
    if (instruction.opcode == ir::Opcode::LoadInt && !FitsImmediate16(instruction.immediate)) {
      const auto next = static_cast<std::uint32_t>(integers.size());
      const auto [entry, inserted] = integer_ids.try_emplace(instruction.immediate, next);
      if (inserted) {
        integers.push_back(instruction.immediate);
      }
      instruction.opcode = ir::Opcode::LoadIntConstant;
      instruction.index = entry->second;
    }
  }
}

std::expected<vm::FunctionStorage, frontend::Diagnostic> EncodeFunction(
    const ir::Function& function,
    EncodingContext context,
    vm::FunctionStorage compiled) {
  if (compiled.native != nullptr && !context.source_paths.empty()) {
    // 原生声明的逻辑调用位置为 0，不构造仅为调用宿主而存在的字节码函数体。
    compiled.source_mappings.push_back({.instruction = 0, .location = function.location});
  }
  auto instructions = function.instructions;
  BindIntegerConstants(instructions, compiled.integers);
  std::vector<std::uint32_t> offsets(instructions.size() + 1);
  const auto has_prefix = [](const ir::Instruction& instruction) {
    const auto bits = vm::IndexOperandBits(instruction.opcode);
    return bits != 0 && instruction.index >= (std::uint32_t{1} << bits);
  };
  for (std::size_t index = 0; index < instructions.size(); ++index) {
    offsets.at(index + 1) = offsets.at(index) + (has_prefix(instructions.at(index)) ? 2U : 1U);
  }
  context.instruction_offsets = offsets;
  compiled.code.reserve(offsets.back());
  for (const auto& instruction : instructions) {
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
    if (has_prefix(instruction)) {
      compiled.code.push_back(vm::EncodeIndexPrefix(instruction.index));
    }
    compiled.code.push_back(*encoded);
    if (!instruction.handler || !ir::AnalyzeEffects(instruction).may_fail) {
      continue;
    }
    const auto target = offsets.at(function.handlers.at(*instruction.handler).target);
    const auto end = static_cast<std::uint32_t>(compiled.code.size());
    if (!compiled.handlers.empty() && compiled.handlers.back().end == begin &&
        compiled.handlers.back().target == target) {
      compiled.handlers.back().end = end;
    } else {
      compiled.handlers.push_back({.begin = begin, .end = end, .target = target});
    }
  }
  return compiled;
}

std::optional<frontend::Diagnostic> AllocateBytecodeRegisters(
    ir::Program& program,
    std::span<const std::string> source_paths) {
  for (auto& function : program.functions) {
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

CompileResult EncodeProgram(ir::Program program,
                            runtime::Isolate& isolate,
                            CompilationState* state) {
  auto native_diagnostics = CheckNativeBindings(program, isolate);
  if (!native_diagnostics.empty()) {
    return {.program = {}, .diagnostics = std::move(native_diagnostics)};
  }
  auto paths = std::move(program.source_paths);
  if (auto failure = AllocateBytecodeRegisters(program, paths)) {
    return {.program = {}, .diagnostics = {std::move(*failure)}};
  }

  CompileResult result;
  runtime::LayoutTransaction layout_transaction(isolate);
  runtime::LayoutCache local_cache;
  auto* cache = state == nullptr ? &local_cache : &state->layouts;
  runtime::LayoutCache pending_cache = *cache;
  if (pending_cache.code_identity == nullptr) {
    pending_cache.code_identity = isolate.CreateCodeIdentity();
  }
  auto layouts = runtime::InstantiateLayouts(program.layouts, isolate, pending_cache);

  std::vector<vm::FunctionStorage> functions;
  auto shared_layouts = std::make_shared<const vm::LayoutTables>(
      std::move(layouts.classes), std::move(layouts.arrays), std::move(layouts.collections),
      std::move(layouts.types), std::move(program.members));
  functions.reserve(program.functions.size());
  const auto source_paths = std::make_shared<const std::vector<std::string>>(std::move(paths));
  std::shared_ptr<vm::StringConstants> strings;
  const auto& literals = program.strings;
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
  for (const ir::Function& function : program.functions) {
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
                                      .source_paths = *source_paths,
                                      .instruction_offsets = {},
                                  },
                                  std::move(compiled));
    if (!encoded) {
      result.diagnostics.push_back(std::move(encoded.error()));
      return result;
    }
    functions.push_back(std::move(*encoded));
  }
  auto prepared =
      vm::Program::Create(std::move(functions), isolate, std::move(program.global_initializers),
                          state != nullptr ? state->globals : nullptr, pending_cache.code_identity);
  if (!prepared) {
    const auto& error = prepared.error();
    const auto& function = program.functions.at(error.function);
    const auto location = error.location.value_or(function.location);
    AddDiagnostic(result.diagnostics, location, error.message);
    return result;
  }
  result.program = std::move(*prepared);
  if (state != nullptr) {
    state->globals = result.program.shared_globals();
  }
  *cache = std::move(pending_cache);
  layout_transaction.Commit();
  return result;
}

}  // namespace

CompileResult CompileParsed(frontend::ParseResult parsed, runtime::Isolate& isolate) {
  auto prepared = ir::PrepareParsed(std::move(parsed));
  if (!prepared) {
    return {.program = {}, .diagnostics = std::move(prepared.error())};
  }
  return EncodeProgram(std::move(*prepared), isolate, nullptr);
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
  ir::PreparationOptions options;
  if (state != nullptr) {
    options.persistent_slots = &state->ids;
    options.declarations = &state->declarations;
    options.revision = ++state->revision;
  }
  auto lowered = ir::PrepareProgram(source, model, options);
  if (!lowered.Succeeded()) {
    return {.program = {}, .diagnostics = std::move(lowered.diagnostics)};
  }
  return EncodeProgram(std::move(lowered.program), isolate, state);
}

}  // namespace vela::compiler
