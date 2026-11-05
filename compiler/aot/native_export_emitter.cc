#include "compiler/aot/native_export_emitter.h"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/Module.h>

#include "compiler/aot/native_compiler.h"
#include "compiler/ir/ir.h"
#include "compiler/native/host_header.h"
#include "compiler/native/native_values.h"

namespace vela::compiler::aot {
namespace {

using native::Builder;

bool IsCIdentifier(std::string_view name) {
  const auto letter = [](char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
  };
  return !name.empty() && (letter(name.front()) || name.front() == '_') &&
         std::ranges::all_of(name, [letter](char value) {
           return letter(value) || value == '_' || (value >= '0' && value <= '9');
         });
}

void EmitExport(llvm::Module& module,
                llvm::Function& image_entry,
                const NativeExport& description) {
  auto& context = module.getContext();
  constexpr unsigned kErrorArgument = 5;
  Builder builder(context);
  auto* pointer = builder.getPtrTy();
  auto* signature = llvm::FunctionType::get(
      builder.getInt8Ty(), {pointer, pointer, pointer, builder.getInt64Ty(), pointer, pointer},
      false);
  auto* function = llvm::Function::Create(signature, llvm::GlobalValue::ExternalLinkage,
                                          description.symbol, module);
  builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", function));
  auto helper = module.getOrInsertFunction("vela_aot_v2_invoke", builder.getInt8Ty(), pointer,
                                           pointer, pointer, builder.getInt32Ty(), pointer,
                                           builder.getInt64Ty(), pointer, pointer);
  builder.CreateRet(builder.CreateCall(helper, {
                                                   function->getArg(0),
                                                   function->getArg(1),
                                                   builder.CreateCall(&image_entry),
                                                   builder.getInt32(description.function),
                                                   function->getArg(2),
                                                   function->getArg(3),
                                                   function->getArg(4),
                                                   function->getArg(kErrorArgument),
                                               }));
}

void CDeclaration(std::ostringstream& output, const NativeExport& description) {
  output << "VelaAotStatus " << description.symbol
         << "(VelaAotScope* scope, VelaAotInstance* instance, const VelaAotValue* arguments, "
            "uint64_t count, VelaAotValue* result, VelaAotError** error);\n";
}

std::expected<void, std::string> CppDeclaration(std::ostringstream& output,
                                                const ir::Program& program,
                                                const ir::Function& source,
                                                const NativeExport& description,
                                                std::string_view name_space) {
  if (!source.signature) {
    return std::unexpected("export has no call signature: " + source.name);
  }
  const auto& signature = *source.signature;
  const auto& layouts = program.layouts;
  const auto result =
      signature.result
          ? native::HostLogicalType(layouts, layouts.types.at(*signature.result), name_space)
          : std::expected<std::string, std::string>("void");
  if (!result) {
    return std::unexpected(result.error());
  }
  std::vector<std::string> parameters;
  for (const auto& parameter : signature.parameters) {
    auto type = layouts.types.at(parameter.type);
    if (parameter.optional) {
      type.nullable = true;
    }
    auto logical = native::HostLogicalType(layouts, type, name_space);
    if (!logical) {
      return std::unexpected(source.name + ": " + logical.error());
    }
    parameters.push_back(std::move(*logical));
  }
  output << "inline vela::Result<vela::HostValue<" << *result << ">> Call_"
         << native::HostIdentifier(description.name)
         << "(vela::HandleScope& scope, VelaAotInstance* instance";
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    output << ", vela::HostValue<" << parameters[index] << "> argument" << index;
  }
  output << ") {\n  return vela::runtime::aot::Invoke<" << *result;
  for (const auto& parameter : parameters) {
    output << ", " << parameter;
  }
  output << ">(scope, instance, &" << description.symbol;
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    output << ", argument" << index;
  }
  output << ");\n}\n";
  return {};
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
  const auto declarations = native::EmitHostDeclarations(program, module_name);
  if (!declarations) {
    return std::unexpected(declarations.error());
  }
  for (const auto& description : exports) {
    EmitExport(module, image_entry, description);
  }
  auto instantiate = std::string(module_name) + "_instantiate";
  auto& context = module.getContext();
  Builder builder(context);
  auto* pointer = builder.getPtrTy();
  auto* signature =
      llvm::FunctionType::get(builder.getInt8Ty(), {pointer, pointer, pointer}, false);
  auto* entry =
      llvm::Function::Create(signature, llvm::GlobalValue::ExternalLinkage, instantiate, module);
  builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", entry));
  auto helper = module.getOrInsertFunction("vela_aot_v2_instantiate", builder.getInt8Ty(), pointer,
                                           pointer, pointer, pointer);
  builder.CreateRet(builder.CreateCall(helper, {
                                                   entry->getArg(0),
                                                   builder.CreateCall(&image_entry),
                                                   entry->getArg(1),
                                                   entry->getArg(2),
                                               }));
  std::ostringstream header;
  header << "#pragma once\n#include \"runtime/native/native_api.h\"\n#ifdef __cplusplus\n"
            "extern \"C\" {\n#endif\nVelaAotStatus "
         << instantiate
         << "(VelaAotIsolate* isolate, VelaAotInstance** instance, VelaAotError** error);\n";
  for (const auto& description : exports) {
    CDeclaration(header, description);
  }
  header << "#ifdef __cplusplus\n}\n#include \"runtime/native/native_api_cpp.h\"\n"
         << *declarations << "namespace " << module_name << " {\n"
         << "inline vela::Result<vela::runtime::aot::Instance> Instantiate(VelaAotIsolate* "
            "isolate) {\n"
            "  VelaAotInstance* instance = nullptr; VelaAotError* error = nullptr;\n"
            "  const auto status = "
         << instantiate
         << "(isolate, &instance, &error);\n"
            "  if (status != VELA_AOT_RETURNED) { return "
            "std::unexpected(vela::runtime::aot::TakeError(status, error)); }\n"
            "  return vela::runtime::aot::Instance(instance);\n}\n";
  for (const auto& description : exports) {
    const auto declared = CppDeclaration(
        header, program, program.functions.at(description.function), description, module_name);
    if (!declared) {
      return std::unexpected(declared.error());
    }
  }
  header << "}\n#endif\n";
  return NativeExportInterface{
      .header = header.str(),
      .instantiate_symbol = std::move(instantiate),
  };
}

}  // namespace vela::compiler::aot
