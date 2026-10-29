#include "compiler/aot/native_export_emitter.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <ostream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constant.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#include "base/execution_status.h"
#include "compiler/aot/native_compiler.h"
#include "compiler/ir/ir.h"
#include "compiler/native/native_values.h"
#include "runtime/native_api.h"

namespace vela::compiler::aot {
namespace {

using native::Builder;
using native::Kind;
using native::NativeRecordType;
using native::SourceType;
using native::Status;

enum class Shape : std::uint8_t { Integer, Double, Boolean, Record, Handle, Void };

bool IsCIdentifier(std::string_view name) {
  const auto letter = [](char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
  };
  return !name.empty() && (letter(name.front()) || name.front() == '_') &&
         std::ranges::all_of(name, [letter](char value) {
           return letter(value) || value == '_' || (value >= '0' && value <= '9');
         });
}

bool IsPublicOptional(const SourceType& type) {
  return type.nullable &&
         (type.kind == Kind::Int || type.kind == Kind::Double || type.kind == Kind::Bool);
}

Shape PublicShape(const SourceType& type) {
  if (type.kind == Kind::Void) {
    return Shape::Void;
  }
  if (type.kind == Kind::Dynamic || type.kind == Kind::Null || IsPublicOptional(type)) {
    return Shape::Record;
  }
  switch (type.kind) {
    case Kind::Int:
      return Shape::Integer;
    case Kind::Double:
      return Shape::Double;
    case Kind::Bool:
      return Shape::Boolean;
    default:
      return Shape::Handle;
  }
}

llvm::Type* ParameterType(Builder& builder, Shape shape) {
  switch (shape) {
    case Shape::Integer:
      return builder.getInt64Ty();
    case Shape::Double:
      return builder.getDoubleTy();
    case Shape::Boolean:
      return builder.getInt8Ty();
    default:
      return builder.getPtrTy();
  }
}

std::string_view CParameterType(Shape shape) {
  switch (shape) {
    case Shape::Integer:
      return "int64_t";
    case Shape::Double:
      return "double";
    case Shape::Boolean:
      return "uint8_t";
    case Shape::Record:
      return "const VelaAotValue*";
    case Shape::Handle:
      return "VelaAotHandle*";
    case Shape::Void:
      return "void";
  }
  std::unreachable();
}

std::string COutputType(Shape shape) {
  return shape == Shape::Record ? "VelaAotValue*" : std::string(CParameterType(shape)) + '*';
}

std::string CppType(const SourceType& type) {
  if (IsPublicOptional(type)) {
    auto required = type;
    required.nullable = false;
    return "std::optional<" + CppType(required) + ">";
  }
  switch (PublicShape(type)) {
    case Shape::Integer:
      return "std::int64_t";
    case Shape::Double:
      return "double";
    case Shape::Boolean:
      return "bool";
    case Shape::Record:
      return "vela::runtime::aot::Value";
    case Shape::Handle:
      return type.kind == Kind::String ? "vela::runtime::aot::String"
                                       : "vela::runtime::aot::Handle<vela::runtime::aot::AnyTag>";
    case Shape::Void:
      return "void";
  }
  std::unreachable();
}

std::string CppName(std::string_view name) {
  constexpr std::string_view kHex = "0123456789abcdef";
  constexpr std::uint8_t kLowNibbleMask = 0x0f;
  std::string result = "Call_";
  for (const auto character : name) {
    const auto byte = static_cast<std::uint8_t>(character);
    if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
        (byte >= '0' && byte <= '9')) {
      result += character;
    } else {
      result += '_';
      result += kHex.at(byte >> 4U);
      result += kHex.at(byte & kLowNibbleMask);
    }
  }
  return result;
}

llvm::Value* PackParameter(Builder& builder, llvm::Value* argument, Shape shape) {
  auto* record_type = NativeRecordType(builder.getContext());
  if (shape == Shape::Record) {
    return builder.CreateLoad(record_type, argument);
  }
  llvm::Value* record = llvm::Constant::getNullValue(record_type);
  llvm::Value* kind = nullptr;
  llvm::Value* bits = argument;
  switch (shape) {
    case Shape::Integer:
      kind = builder.getInt8(VELA_AOT_INTEGER);
      break;
    case Shape::Double:
      kind = builder.getInt8(VELA_AOT_DOUBLE);
      bits = builder.CreateBitCast(argument, builder.getInt64Ty());
      break;
    case Shape::Boolean:
      kind = builder.getInt8(VELA_AOT_BOOLEAN);
      bits = builder.CreateZExt(argument, builder.getInt64Ty());
      break;
    default:
      kind = builder.CreateSelect(builder.CreateIsNotNull(argument),
                                  builder.getInt8(VELA_AOT_HANDLE), builder.getInt8(VELA_AOT_NULL));
      bits = builder.CreatePtrToInt(argument, builder.getInt64Ty());
      break;
  }
  record = builder.CreateInsertValue(record, bits, 0);
  return builder.CreateInsertValue(record, kind, 1);
}

void WriteResult(Builder& builder, llvm::Value* record, Shape shape, llvm::Value* output) {
  llvm::Value* value = record;
  if (shape != Shape::Record) {
    value = builder.CreateExtractValue(record, 0);
    if (shape == Shape::Double) {
      value = builder.CreateBitCast(value, builder.getDoubleTy());
    } else if (shape == Shape::Boolean) {
      value = builder.CreateTrunc(value, builder.getInt8Ty());
    } else if (shape == Shape::Handle) {
      value = builder.CreateIntToPtr(value, builder.getPtrTy());
    }
  }
  builder.CreateStore(value, output);
}

std::expected<void, std::string> EmitExport(llvm::Module& module,
                                            llvm::Function& image_entry,
                                            const ir::Function& source,
                                            const NativeExport& description) {
  const auto count = source.parameter_count;
  if ((static_cast<std::uint64_t>(count) + 1) * sizeof(VelaAotValue) >
      native::kMaximumNativeStackBytes) {
    return std::unexpected("AOT public wrapper arguments exceed the native stack budget");
  }
  auto& context = module.getContext();
  Builder builder(context);
  std::vector<llvm::Type*> parameters{builder.getPtrTy()};
  for (std::size_t index = 0; index < count; ++index) {
    parameters.push_back(ParameterType(builder, PublicShape(source.value_types.at(index))));
  }
  const auto output_shape = PublicShape(source.return_type);
  if (output_shape != Shape::Void) {
    parameters.push_back(builder.getPtrTy());
  }
  parameters.push_back(builder.getPtrTy());
  auto* signature = llvm::FunctionType::get(builder.getInt8Ty(), parameters, false);
  auto* function = llvm::Function::Create(signature, llvm::GlobalValue::ExternalLinkage,
                                          description.symbol, module);
  builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", function));
  llvm::Value* valid = builder.getTrue();
  llvm::Value* output = nullptr;
  if (output_shape != Shape::Void) {
    output = function->getArg(count + 1);
    valid = builder.CreateIsNotNull(output);
  }
  for (std::size_t index = 0; index < count; ++index) {
    if (PublicShape(source.value_types.at(index)) == Shape::Record) {
      valid = builder.CreateAnd(
          valid, builder.CreateIsNotNull(function->getArg(static_cast<unsigned>(index + 1))));
    }
  }
  auto* invoke = llvm::BasicBlock::Create(context, "invoke", function);
  auto* invalid = llvm::BasicBlock::Create(context, "invalid", function);
  builder.CreateCondBr(valid, invoke, invalid);
  builder.SetInsertPoint(invalid);
  builder.CreateRet(Status(builder, ExecutionStatus::InvalidEntry));
  builder.SetInsertPoint(invoke);
  auto* record_type = NativeRecordType(context);
  auto* arguments = builder.CreateAlloca(record_type, builder.getInt64(count));
  auto* result = builder.CreateAlloca(record_type);
  for (std::size_t index = 0; index < count; ++index) {
    auto* slot = builder.CreateConstGEP1_64(record_type, arguments, index);
    auto* value = PackParameter(builder, function->getArg(static_cast<unsigned>(index + 1)),
                                PublicShape(source.value_types.at(index)));
    builder.CreateStore(value, slot);
  }
  auto helper =
      module.getOrInsertFunction("vela_aot_v1_invoke", builder.getInt8Ty(), builder.getPtrTy(),
                                 builder.getPtrTy(), builder.getInt32Ty(), builder.getPtrTy(),
                                 builder.getInt64Ty(), builder.getPtrTy(), builder.getPtrTy());
  auto* status = builder.CreateCall(
      helper, {
                  function->getArg(0),
                  builder.CreateCall(&image_entry),
                  builder.getInt32(description.function),
                  arguments,
                  builder.getInt64(count),
                  result,
                  function->getArg(static_cast<unsigned>(function->arg_size() - 1)),
              });
  auto* returned = llvm::BasicBlock::Create(context, "returned", function);
  auto* failed = llvm::BasicBlock::Create(context, "failed", function);
  builder.CreateCondBr(builder.CreateICmpEQ(status, Status(builder, ExecutionStatus::Returned)),
                       returned, failed);
  builder.SetInsertPoint(failed);
  builder.CreateRet(status);
  builder.SetInsertPoint(returned);
  if (output_shape != Shape::Void) {
    WriteResult(builder, builder.CreateLoad(record_type, result), output_shape, output);
  }
  builder.CreateRet(status);
  return {};
}

void CDeclaration(std::ostream& output,
                  const ir::Function& source,
                  const NativeExport& description) {
  output << "VelaAotStatus " << description.symbol << "(VelaAotInstance* instance";
  for (std::size_t index = 0; index < source.parameter_count; ++index) {
    output << ", " << CParameterType(PublicShape(source.value_types.at(index))) << " argument"
           << index;
  }
  const auto shape = PublicShape(source.return_type);
  if (shape != Shape::Void) {
    output << ", " << COutputType(shape) << " result";
  }
  output << ", VelaAotHandle** error);\n";
}

void CppDeclaration(std::ostream& output,
                    const ir::Function& source,
                    const NativeExport& description) {
  const auto result_type = CppType(source.return_type);
  output << "inline vela::runtime::aot::Result<" << result_type << "> " << CppName(description.name)
         << "(VelaAotInstance* instance";
  std::vector<std::string> arguments;
  for (std::size_t index = 0; index < source.parameter_count; ++index) {
    const auto& type = source.value_types.at(index);
    const auto shape = PublicShape(type);
    const auto argument = "argument" + std::to_string(index);
    const auto borrowed =
        shape == Shape::Handle || (shape == Shape::Record && !IsPublicOptional(type));
    output << ", " << (borrowed ? "const " : "") << CppType(type) << (borrowed ? "& " : " ")
           << argument;
    arguments.push_back(argument);
  }
  output << ") {\n";
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const auto& type = source.value_types.at(index);
    const auto shape = PublicShape(type);
    auto& argument = arguments.at(index);
    if (shape == Shape::Handle) {
      argument += ".get()";
    } else if (shape == Shape::Record) {
      output << "  const auto encoded" << index << " = "
             << (IsPublicOptional(type) ? "vela::runtime::aot::Encode(" + argument + ")"
                                        : argument + ".raw()")
             << ";\n";
      argument = "&encoded" + std::to_string(index);
    } else if (shape == Shape::Boolean) {
      argument.insert(0, "static_cast<uint8_t>(");
      argument += ')';
    }
  }
  const auto shape = PublicShape(source.return_type);
  if (shape != Shape::Void) {
    auto type = COutputType(shape);
    type.pop_back();
    output << "  " << type << " result{};\n";
  }
  output << "  VelaAotHandle* error = nullptr;\n  const auto status = " << description.symbol
         << "(instance";
  for (const auto& argument : arguments) {
    output << ", " << argument;
  }
  if (shape != Shape::Void) {
    output << ", &result";
  }
  output << ", &error);\n  if (status != VELA_AOT_RETURNED) { return "
            "vela::runtime::aot::Failure(status, error); }\n";
  switch (shape) {
    case Shape::Handle:
      output << "  return " << result_type << "::Adopt(result);\n";
      break;
    case Shape::Void:
      output << "  return {};\n";
      break;
    case Shape::Boolean:
      output << "  return result != 0;\n";
      break;
    case Shape::Record:
      if (IsPublicOptional(source.return_type)) {
        auto required = source.return_type;
        required.nullable = false;
        output << "  return vela::runtime::aot::DecodeNullable<" << CppType(required)
               << ">(result);\n";
      } else {
        output << "  return vela::runtime::aot::Value::Adopt(result);\n";
      }
      break;
    default:
      output << "  return result;\n";
      break;
  }
  output << "}\n";
}

}  // namespace

std::expected<NativeExportInterface, std::string> EmitNativeExports(
    llvm::Module& module,
    const ir::Program& program,
    std::span<const NativeExport> exports,
    llvm::Function& image_entry,
    std::string_view module_name) {
  if (!IsCIdentifier(module_name)) {
    return std::unexpected("AOT module name must be a C identifier");
  }
  auto& context = module.getContext();
  Builder builder(context);
  auto* record = NativeRecordType(context);
  const auto* layout = module.getDataLayout().getStructLayout(record);
  const std::array offsets{
      offsetof(VelaAotValue, payload),       offsetof(VelaAotValue, kind),
      offsetof(VelaAotValue, reserved_byte), offsetof(VelaAotValue, reserved_half),
      offsetof(VelaAotValue, reserved_word),
  };
  for (std::size_t index = 0; index < offsets.size(); ++index) {
    if (layout->getElementOffset(static_cast<unsigned>(index)) != offsets.at(index)) {
      return std::unexpected("native public value layout does not match target data layout");
    }
  }
  for (const auto& description : exports) {
    const auto emitted =
        EmitExport(module, image_entry, program.functions.at(description.function), description);
    if (!emitted) {
      return std::unexpected(emitted.error());
    }
  }
  auto instantiate = std::string(module_name) + "_instantiate";
  auto* pointer = builder.getPtrTy();
  auto* signature =
      llvm::FunctionType::get(builder.getInt8Ty(), {pointer, pointer, pointer}, false);
  auto* entry =
      llvm::Function::Create(signature, llvm::GlobalValue::ExternalLinkage, instantiate, module);
  builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", entry));
  auto* image = builder.CreateCall(&image_entry);
  auto helper = module.getOrInsertFunction("vela_aot_v1_instantiate", builder.getInt8Ty(), pointer,
                                           pointer, pointer, pointer);
  builder.CreateRet(
      builder.CreateCall(helper, {entry->getArg(0), image, entry->getArg(1), entry->getArg(2)}));
  std::ostringstream header;
  header << "#pragma once\n#include \"runtime/native_api.h\"\n#ifdef __cplusplus\nextern \"C\" "
            "{\n#endif\n"
         << "VelaAotStatus " << instantiate
         << "(VelaAotIsolate* isolate, VelaAotInstance** instance, VelaAotHandle** error);\n";
  for (const auto& description : exports) {
    CDeclaration(header, program.functions.at(description.function), description);
  }
  header << "#ifdef __cplusplus\n}\n#include \"runtime/native_api_cpp.h\"\nnamespace "
         << module_name << " {\n"
         << "inline vela::runtime::aot::Result<vela::runtime::aot::Instance> "
            "Instantiate(VelaAotIsolate* isolate) {\n"
         << "  VelaAotInstance* instance = nullptr; VelaAotHandle* error = nullptr;\n"
         << "  const auto status = " << instantiate << "(isolate, &instance, &error);\n"
         << "  if (status != VELA_AOT_RETURNED) { return vela::runtime::aot::Failure(status, "
            "error); }\n"
         << "  return vela::runtime::aot::Instance(instance);\n}\n";
  for (const auto& description : exports) {
    CppDeclaration(header, program.functions.at(description.function), description);
  }
  header << "}\n#endif\n";
  return NativeExportInterface{
      .header = header.str(),
      .instantiate_symbol = std::move(instantiate),
  };
}

}  // namespace vela::compiler::aot
