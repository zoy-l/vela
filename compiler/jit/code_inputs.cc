#include "compiler/jit/code_inputs.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "base/binary.h"
#include "base/callable.h"
#include "base/fingerprint.h"
#include "base/layout_description.h"
#include "base/native_program_mode.h"
#include "base/opcode.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/ir/ir.h"
#include "compiler/native/call_targets.h"
#include "compiler/native/field_access.h"

namespace vela::compiler::jit {
namespace {

template <class T>
  requires std::is_integral_v<T> || std::is_enum_v<T>
void Put(BinaryWriter& writer, T value) {
  writer.Write(static_cast<std::uint64_t>(value));
}

void Put(BinaryWriter& writer, const frontend::TypeDescriptor& type) {
  Put(writer, type.kind);
  Put(writer, type.nullable);
  writer.Write(type.name);
}

void Put(BinaryWriter& writer, const layout::Type& type) {
  Put(writer, type.kind);
  Put(writer, type.nullable);
  Put(writer, type.index);
}

template <class T, class Write>
void Sequence(BinaryWriter& writer, const std::vector<T>& values, Write write) {
  Put(writer, values.size());
  for (const auto& value : values) {
    write(writer, value);
  }
}

template <class T>
void Numbers(BinaryWriter& writer, const std::vector<T>& values) {
  Sequence(writer, values, [](BinaryWriter& output, T value) { Put(output, value); });
}

void Signature(BinaryWriter& writer, const ir::Function& function, const ir::Program& program) {
  writer.Write(function.native_symbol);
  Put(writer, function.return_type);
  Put(writer, function.parameter_count);
  Put(writer, function.is_async);
  Put(writer, function.is_generated_constructor);
  Put(writer, function.identity.declaration.value);
  Put(writer, function.identity.local);
  for (std::size_t index = 0; index < function.parameter_count; ++index) {
    Put(writer, function.value_types.at(index));
  }
  // 普通函数的公开签名表由当前映像解释；表内编号不参与机器调用约定。
  // 外部函数的签名形状还参与内建身份识别，属于真实代码生成输入。
  if (function.native_symbol.empty()) {
    return;
  }
  Put(writer, function.signature.has_value());
  if (function.signature) {
    const auto& signature = *function.signature;
    Put(writer, signature.hidden_parameters);
    Put(writer, signature.result.value_or(kInvalidFunctionTarget));
    if (signature.result) {
      Put(writer, program.layouts.types.at(*signature.result));
    }
    Sequence(writer, signature.parameters, [&](BinaryWriter& output, const auto& parameter) {
      output.Write(parameter.name);
      Put(output, parameter.type);
      Put(output, parameter.optional);
      Put(output, parameter.rest);
      Put(output, program.layouts.types.at(parameter.type));
    });
  }
}

std::vector<std::string> ClassInputs(const ir::Program& program) {
  std::vector<std::string> fingerprints;
  for (std::size_t index = 0; index < program.classes.size(); ++index) {
    const auto& type = program.classes.at(index);
    const auto& layout = program.layouts.classes.at(index);
    BinaryWriter output;
    output.Write(type.name);
    Put(output, type.kind);
    Put(output, type.declaration.value);
    Put(output, type.base_class.value);
    Sequence(output, type.field_types,
             [](BinaryWriter& fields, const auto& field) { Put(fields, field); });
    Sequence(output, type.fields, [](BinaryWriter& fields, const auto& field) {
      Put(fields, field.declaration.value);
      Put(fields, field.type);
      Put(fields, field.initializer);
      Put(fields, field.visibility);
    });
    Put(output, type.callable.has_value());
    if (type.callable) {
      Put(output, type.callable->function);
      Put(output, type.callable->mode);
    }
    Numbers(output, type.interfaces);
    Numbers(output, type.dispatch_targets);
    output.Write(type.callable_type);
    Put(output, layout.kind);
    Put(output, layout.field_count);
    Numbers(output, layout.reference_fields);
    Put(output, layout.callable_type.value_or(kInvalidFunctionTarget));
    fingerprints.push_back(ContentFingerprint(output.bytes()).Hex());
  }
  return fingerprints;
}

std::vector<std::string> SignatureInputs(const ir::Program& program) {
  std::vector<std::string> signatures;
  signatures.reserve(program.functions.size());
  for (const auto& function : program.functions) {
    BinaryWriter writer;
    Signature(writer, function, program);
    signatures.push_back(ContentFingerprint(writer.bytes()).Hex());
  }
  return signatures;
}

void InstructionLayouts(BinaryWriter& writer,
                        const ir::Instruction& instruction,
                        const ir::Program& program,
                        const std::vector<std::string>& classes) {
  const auto& layouts = program.layouts;
  if (instruction.array_type != ir::kInvalidArrayType) {
    const auto& array = layouts.arrays.at(layouts.array_types.at(instruction.array_type));
    writer.Write(array.name);
    Put(writer, array.element);
  }
  switch (instruction.opcode) {
    case Opcode::NewInstance:
      writer.Write(classes.at(instruction.index));
      break;
    case Opcode::NewCollection: {
      const auto& collection =
          layouts.collections.at(layouts.collection_types.at(instruction.index));
      writer.Write(collection.name);
      Put(writer, collection.key);
      Put(writer, collection.value.has_value());
      if (collection.value) {
        Put(writer, *collection.value);
      }
      break;
    }
    case Opcode::CheckedCast:
    case Opcode::IsType:
      Put(writer, layouts.types.at(instruction.index));
      break;
    case Opcode::GetGlobal:
    case Opcode::SetGlobal:
      Put(writer, program.global_declarations.at(instruction.index).value);
      break;
    case Opcode::CallInterface:
      Put(writer, program.selector_declarations.at(instruction.index).value);
      break;
    default:
      break;
  }
}

void Instruction(BinaryWriter& writer, const ir::Instruction& instruction) {
  Put(writer, instruction.opcode);
  Put(writer, instruction.dst);
  Put(writer, instruction.lhs);
  Put(writer, instruction.rhs);
  Put(writer, instruction.immediate);
  Put(writer, instruction.index);
  Put(writer, instruction.location.line);
  Put(writer, instruction.location.column);
  Put(writer, instruction.location.source_id);
  Put(writer, instruction.handler.value_or(kInvalidFunctionTarget));
  Put(writer, instruction.array_type);
  Put(writer, instruction.field_declaration.value_or(DeclarationId{}).value);
  Put(writer, instruction.binding_receiver.value_or(DeclarationId{}).value);
  Put(writer, instruction.construction.has_value());
  if (instruction.construction) {
    const auto& construction = *instruction.construction;
    Put(writer, construction.type.value);
    Put(writer, construction.generated);
    Sequence(writer, construction.supplied,
             [](BinaryWriter& output, const auto declaration) { Put(output, declaration.value); });
  }
  Put(writer, instruction.completes_construction);
}

void InstructionInputs(BinaryWriter& writer,
                       const ir::Function& function,
                       const ir::Program& program,
                       NativeCodeTier tier,
                       const std::vector<std::string>& classes,
                       const std::vector<std::string>& signatures,
                       const absl::flat_hash_map<DeclarationId, native::FieldTarget>& fields) {
  Put(writer, function.instructions.size());
  for (const auto& instruction : function.instructions) {
    Instruction(writer, instruction);
    InstructionLayouts(writer, instruction, program, classes);
    if (instruction.field_declaration && tier == NativeCodeTier::Optimized &&
        (instruction.opcode == Opcode::GetField || instruction.opcode == Opcode::SetField)) {
      const auto target = fields.at(*instruction.field_declaration);
      Put(writer, target.owner);
      Put(writer, target.slot);
      writer.Write(classes.at(target.owner));
    }
    if (instruction.opcode == Opcode::Call) {
      writer.Write(signatures.at(instruction.index));
    }
  }
}

}  // namespace

std::vector<FunctionCodeInputs> DescribeCodeInputs(const ir::Program& program,
                                                   const native::NativeCallPlan& calls,
                                                   NativeCodeTier tier,
                                                   std::uint8_t optimization,
                                                   std::uint8_t debugging,
                                                   std::uint8_t escape) {
  const auto classes = ClassInputs(program);
  const auto signatures = SignatureInputs(program);
  const auto fields = native::FindFieldTargets(program);
  absl::flat_hash_map<std::string_view, std::size_t> class_indices;
  for (std::size_t index = 0; index < program.classes.size(); ++index) {
    class_indices.emplace(program.classes.at(index).name, index);
  }
  std::vector<FunctionCodeInputs> inputs;
  for (const auto& function : program.functions) {
    BinaryWriter writer;
    writer.Write(function.name);
    Put(writer, tier);
    Put(writer, optimization);
    Put(writer, debugging);
    Put(writer, escape);
    writer.Write(signatures.at(inputs.size()));
    Put(writer, function.register_count);
    Put(writer, function.is_exported);
    Sequence(writer, function.value_types, [&](BinaryWriter& output, const auto& type) {
      Put(output, type);
      const auto name = type.nullable ? type.name.substr(0, type.name.size() - 1) : type.name;
      const auto found = class_indices.find(name);
      Put(output, found != class_indices.end());
      if (found != class_indices.end()) {
        Put(output, found->second);
        output.Write(classes.at(found->second));
      }
    });
    Sequence(writer, function.doubles, [](BinaryWriter& output, double value) {
      output.Write(std::bit_cast<std::uint64_t>(value));
    });
    Sequence(writer, function.handlers,
             [](BinaryWriter& output, const auto& handler) { Put(output, handler.target); });
    FunctionCodeInputs input;
    InstructionInputs(writer, function, program, tier, classes, signatures, fields);
    input.dependencies = calls.dependencies.at(inputs.size());
    input.fingerprint = ContentFingerprint(writer.bytes()).Hex();
    inputs.push_back(std::move(input));
  }
  return inputs;
}

}  // namespace vela::compiler::jit
