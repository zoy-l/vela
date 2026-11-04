#include "compiler/bindgen/model.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/JSON.h>

#include "base/native_binding.h"

namespace vela::bindgen {
namespace {

llvm::json::Object SerializeSource(const Source& source) {
  return llvm::json::Object{
      {.K = "path", .V = source.path},
      {.K = "line", .V = source.line},
      {.K = "column", .V = source.column},
  };
}

std::string_view TypeName(TypeKind kind) {
  switch (kind) {
    case TypeKind::Void:
      return "void";
    case TypeKind::Boolean:
      return "bool";
    case TypeKind::Integer:
      return "int";
    case TypeKind::Double:
      return "double";
    case TypeKind::String:
      return "string";
    case TypeKind::Cancellation:
      return "CancellationToken";
    case TypeKind::Host:
      return "host";
  }
  llvm_unreachable("exhaustive type kind");
}

std::string_view OwnershipName(Ownership ownership) {
  switch (ownership) {
    case Ownership::Value:
      return "value";
    case Ownership::Borrow:
      return "borrow";
    case Ownership::Unique:
      return "unique";
    case Ownership::Shared:
      return "shared";
  }
  llvm_unreachable("exhaustive ownership");
}

llvm::json::Object SerializeType(const Type& type) {
  return llvm::json::Object{
      {.K = "kind", .V = std::string(TypeName(type.kind))},
      {.K = "key", .V = type.key},
      {.K = "cpp", .V = type.cpp},
      {.K = "ownership", .V = std::string(OwnershipName(type.ownership))},
      {.K = "nullable", .V = type.nullable},
      {.K = "const", .V = type.is_const},
  };
}

llvm::json::Object SerializeMember(const Member& member) {
  llvm::json::Array parameters;
  for (const auto& parameter : member.parameters) {
    parameters.push_back(llvm::json::Object{
        {.K = "name", .V = parameter.name},
        {.K = "type", .V = SerializeType(parameter.type)},
        {.K = "source", .V = SerializeSource(parameter.source)},
    });
  }
  return llvm::json::Object{
      {.K = "kind", .V = std::string(NativeMemberKindName(member.kind))},
      {.K = "name", .V = member.name},
      {.K = "key", .V = member.key},
      {.K = "cpp_name", .V = member.cpp_name},
      {.K = "cpp_pointer_type", .V = member.cpp_pointer_type},
      {.K = "documentation", .V = member.documentation},
      {.K = "parameters", .V = std::move(parameters)},
      {.K = "result", .V = SerializeType(member.result)},
      {.K = "source", .V = SerializeSource(member.source)},
      {.K = "context", .V = member.context},
      {.K = "fallible", .V = member.fallible},
      {.K = "const", .V = member.is_const},
  };
}

}  // namespace

std::string PublicContract(const Class& type) {
  const auto contract_type = [](const Type& value) {
    return NativeContractType{
        .key = value.kind == TypeKind::Host ? value.key : std::string(TypeName(value.kind)),
        .nullable = value.nullable,
    };
  };
  std::vector<NativeContractMember> members;
  for (const auto& source : type.members) {
    NativeContractMember member{
        .key = source.key,
        .parameters = {},
        .result = contract_type(source.result),
    };
    for (const auto& parameter : source.parameters) {
      member.parameters.push_back({.name = parameter.name, .type = contract_type(parameter.type)});
    }
    members.push_back(std::move(member));
  }
  return NativeClassContract(std::move(members)).Hex();
}

std::string RegistrationNamespace(std::string_view module) {
  // Vela 模块名可以是 C++ 关键字；前缀和可逆转义也避免生成保留的双下划线。
  std::string result = "vela::bindings::module_";
  for (const char character : module) {
    if (character == '_') {
      result += "Z_";
    } else if (character == 'Z') {
      result += "ZZ";
    } else {
      result += character;
    }
  }
  return result;
}

llvm::json::Object Serialize(const Module& module) {
  llvm::json::Object inputs;
  for (const auto& [path, fingerprint] : module.input_fingerprints) {
    inputs[path] = fingerprint;
  }
  llvm::json::Array classes;
  for (const auto& type : module.classes) {
    llvm::json::Array members;
    for (const auto& member : type.members) {
      members.push_back(SerializeMember(member));
    }
    classes.push_back(llvm::json::Object{
        {.K = "key", .V = type.key},
        {.K = "contract", .V = PublicContract(type)},
        {.K = "name", .V = type.name},
        {.K = "cpp", .V = type.cpp},
        {.K = "documentation", .V = type.documentation},
        {.K = "source", .V = SerializeSource(type.source)},
        {.K = "members", .V = std::move(members)},
    });
  }
  return llvm::json::Object{
      {.K = "format_version", .V = 1},
      {.K = "module", .V = module.name},
      {.K = "registration", .V = RegistrationNamespace(module.name) + "::Register"},
      {.K = "target", .V = module.target},
      {.K = "clang_version", .V = module.clang_version},
      {.K = "directory", .V = module.directory},
      {.K = "arguments", .V = llvm::json::Array(module.arguments)},
      {.K = "inputs", .V = llvm::json::Array(module.inputs)},
      {.K = "input_fingerprints", .V = std::move(inputs)},
      {.K = "classes", .V = std::move(classes)},
  };
}

}  // namespace vela::bindgen
