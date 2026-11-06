#include "compiler/bindgen/model.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/JSON.h>

#include "base/layout_description.h"
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

std::string_view TypeName(const layout::Program& program, const layout::Type& type) {
  using Kind = layout::Type::Kind;
  switch (type.kind) {
    case Kind::Dynamic:
    case Kind::Boolean:
    case Kind::Integer:
    case Kind::Double:
    case Kind::String:
    case Kind::CancellationToken:
    case Kind::MutableString:
    case Kind::ByteArray:
    case Kind::ByteData:
    case Kind::RegExp:
    case Kind::RegExpMatch:
    case Kind::Timer:
      return layout::ScalarTypeName(type.kind);
    case Kind::Instance:
      return program.classes.at(type.index).kind == layout::Class::Kind::Host ? "host" : "object";
    case Kind::Enum:
      return "enum";
    case Kind::Interface:
      return "interface";
    case Kind::Array:
      return "array";
    case Kind::Collection:
      return program.collections.at(type.index).value ? "map" : "set";
    case Kind::Callable:
      return "function";
    case Kind::Future:
      return "future";
    case Kind::Completer:
      return "completer";
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
    case Ownership::Local:
      return "local";
  }
  llvm_unreachable("exhaustive ownership");
}

llvm::json::Object SerializeShape(const layout::Program& program,
                                  std::optional<layout::Type> type) {
  if (!type) {
    return llvm::json::Object{
        {.K = "kind", .V = "void"},
        {.K = "key", .V = ""},
        {.K = "nullable", .V = false},
    };
  }
  llvm::json::Object result{
      {.K = "kind", .V = std::string(TypeName(program, *type))},
      {.K = "key", .V = ""},
      {.K = "nullable", .V = type->nullable},
  };
  const auto describe_origin = [&](const std::string& key) {
    const auto& origin = program.nominals.at(key);
    result["module"] = origin.module;
    result["declaration"] = origin.name;
    llvm::json::Array arguments;
    for (const auto& argument : origin.arguments) {
      arguments.push_back(SerializeShape(program, argument));
    }
    result["arguments"] = std::move(arguments);
  };
  using Kind = layout::Type::Kind;
  switch (type->kind) {
    case Kind::Instance:
    case Kind::Enum: {
      const auto& nominal = program.classes.at(type->index);
      result["key"] = nominal.native_key.empty() ? nominal.name : nominal.native_key;
      if (nominal.kind != layout::Class::Kind::Host) {
        describe_origin(nominal.name);
      }
      break;
    }
    case Kind::Interface: {
      const auto& key = program.interfaces.at(type->index);
      result["key"] = key;
      describe_origin(key);
      break;
    }
    case Kind::Array:
      result["element"] = SerializeShape(program, program.arrays.at(type->index).element);
      break;
    case Kind::Collection: {
      const auto& collection = program.collections.at(type->index);
      result["key_type"] = SerializeShape(program, collection.key);
      if (collection.value) {
        result["value_type"] = SerializeShape(program, collection.value);
      }
      break;
    }
    case Kind::Callable:
    case Kind::Future:
    case Kind::Completer: {
      const auto& signature = program.signatures.at(type->index);
      llvm::json::Array parameters;
      for (const auto& parameter : signature.parameters) {
        parameters.push_back(SerializeShape(program, parameter.type));
      }
      result["parameters"] = std::move(parameters);
      result["result"] = SerializeShape(program, signature.result);
      break;
    }
    default:
      break;
  }
  return result;
}

llvm::json::Object SerializeType(const Type& type) {
  auto result = SerializeShape(type.layouts, type.value);
  result["cpp"] = type.cpp;
  result["ownership"] = std::string(OwnershipName(type.ownership));
  result["const"] = type.is_const;
  return result;
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

std::string Type::ContractKey() const {
  return value ? layout::TypeKey(layouts, *value) : "void";
}

bool Type::IsSameLogicalType(const Type& other) const {
  return value.has_value() == other.value.has_value() &&
         (!value || layout::IsSameType(layouts, *value, other.layouts, *other.value));
}

Type HostType(std::string_view key, Ownership ownership) {
  Type result{
      .layouts = {},
      .value = layout::Type{.kind = layout::Type::Kind::Instance},
      .ownership = ownership,
      .cpp = {},
  };
  result.layouts.classes.push_back({
      .name = {},
      .native_key = std::string(key),
      .native_contract = {},
      .kind = layout::Class::Kind::Host,
      .reference_fields = {},
      .callable = {},
      .interfaces = {},
      .dispatch_targets = {},
  });
  return result;
}

std::string PublicContract(const Class& type) {
  std::vector<NativeContractMember> members;
  for (const auto& source : type.members) {
    NativeContractMember member{
        .key = source.key,
        .parameters = {},
        .result = source.result.ContractKey(),
    };
    for (const auto& parameter : source.parameters) {
      member.parameters.push_back({.name = parameter.name, .type = parameter.type.ContractKey()});
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
  llvm::json::Array paths;
  for (const auto& [path, fingerprint] : module.input_fingerprints) {
    inputs[path] = fingerprint;
    paths.push_back(path);
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
      {.K = "format_version", .V = 2},
      {.K = "module", .V = module.name},
      {.K = "registration", .V = RegistrationNamespace(module.name) + "::Register"},
      {.K = "target", .V = module.target},
      {.K = "clang_version", .V = module.clang_version},
      {.K = "directory", .V = module.directory},
      {.K = "arguments", .V = llvm::json::Array(module.arguments)},
      {.K = "inputs", .V = std::move(paths)},
      {.K = "input_fingerprints", .V = std::move(inputs)},
      {.K = "classes", .V = std::move(classes)},
  };
}

}  // namespace vela::bindgen
