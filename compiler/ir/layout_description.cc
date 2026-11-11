#include "compiler/ir/layout_description.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include <absl/container/flat_hash_map.h>

#include "base/callable.h"
#include "base/layout_description.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/ir/ir.h"

namespace vela::ir {
namespace {

using SourceType = frontend::TypeDescriptor;

bool HasReference(SourceType::Kind kind) {
  using Kind = SourceType::Kind;
  return kind != Kind::Invalid && kind != Kind::Null && kind != Kind::Void && kind != Kind::Bool &&
         kind != Kind::Int && kind != Kind::Double && kind != Kind::Enum;
}

class LayoutDescription final {
 public:
  LayoutDescription(const Program& program,
                    const frontend::SemanticModel& model,
                    const frontend::Program& source)
      : program_(&program), model_(&model), source_(&source) {}

  layout::Program Build();

 private:
  layout::Type Type(const SourceType& type);
  std::uint32_t Array(const SourceType& type);
  std::uint32_t Collection(const SourceType& type);
  std::uint32_t Signature(const SourceType& type);
  std::uint32_t CallableSignature(const Class& type);
  layout::Class DescribeClass(const Class& type);
  void Canonicalize(layout::Type type);
  std::string Nominal(const std::string& name);

  const Program* program_;
  const frontend::SemanticModel* model_;
  const frontend::Program* source_;
  layout::Program result_;
  absl::flat_hash_map<std::string, std::uint32_t> classes_;
  absl::flat_hash_map<std::string, std::uint32_t> arrays_;
  absl::flat_hash_map<std::string, std::uint32_t> collections_;
  absl::flat_hash_map<std::string, std::uint32_t> signatures_;
  absl::flat_hash_map<std::string, std::string> nominal_keys_;
  absl::flat_hash_map<std::string, bool> exported_;
};

std::string LayoutDescription::Nominal(const std::string& name) {
  if (const auto found = nominal_keys_.find(name); found != nominal_keys_.end()) {
    return found->second;
  }
  const auto origin = source_->origins.find(name);
  if (origin == source_->origins.end()) {
    // 捕获单元和 callable class 没有源码名义身份，只由其函数版本拥有。
    return name;
  }
  const auto& declaration = origin->second;
  layout::Nominal nominal{
      .module = declaration.module_uri,
      .name = declaration.name,
      .arguments = {},
      .exported = exported_.at(name),
  };
  if (!declaration.arguments.empty()) {
    for (const auto& argument : model_->generic_arguments.at(name)) {
      const auto type = Type(argument);
      Canonicalize(type);
      nominal.arguments.push_back(type);
    }
  }
  auto key = layout::NominalKey(result_, nominal);
  result_.nominals.emplace(key, std::move(nominal));
  nominal_keys_.emplace(name, key);
  return key;
}

void LayoutDescription::Canonicalize(layout::Type type) {
  using Kind = layout::Type::Kind;
  switch (type.kind) {
    case Kind::Instance:
    case Kind::Enum: {
      auto& nominal = result_.classes.at(type.index);
      nominal.name = Nominal(program_->classes.at(type.index).name);
      break;
    }
    case Kind::Interface: {
      auto& name = result_.interfaces.at(type.index);
      // 已规范化的接口不再通过语义表查询源码名。
      if (!result_.nominals.contains(name)) {
        name = Nominal(name);
      }
      break;
    }
    case Kind::Array:
      Canonicalize(result_.arrays.at(type.index).element);
      break;
    case Kind::Collection: {
      const auto collection = result_.collections.at(type.index);
      Canonicalize(collection.key);
      if (collection.value) {
        Canonicalize(*collection.value);
      }
      break;
    }
    case Kind::Callable:
    case Kind::Future:
    case Kind::Completer: {
      const auto signature = result_.signatures.at(type.index);
      for (const auto& parameter : signature.parameters) {
        Canonicalize(parameter.type);
      }
      if (signature.result) {
        Canonicalize(*signature.result);
      }
      break;
    }
    default:
      break;
  }
}

layout::Type LayoutDescription::Type(const SourceType& type) {
  using Source = SourceType::Kind;
  using Kind = layout::Type::Kind;
  layout::Type result{.nullable = type.nullable};
  switch (type.kind) {
    case Source::Dynamic:
      result.kind = Kind::Dynamic;
      break;
    case Source::Bool:
      result.kind = Kind::Boolean;
      break;
    case Source::Int:
      result.kind = Kind::Integer;
      break;
    case Source::Double:
      result.kind = Kind::Double;
      break;
    case Source::String:
      result.kind = Kind::String;
      break;
    case Source::MutableString:
      result.kind = Kind::MutableString;
      break;
    case Source::ByteArray:
      result.kind = Kind::ByteArray;
      break;
    case Source::ByteData:
      result.kind = Kind::ByteData;
      break;
    case Source::RegExp:
      result.kind = Kind::RegExp;
      break;
    case Source::RegExpMatch:
      result.kind = Kind::RegExpMatch;
      break;
    case Source::Class:
    case Source::Enum:
      result.kind = type.kind == Source::Enum ? Kind::Enum : Kind::Instance;
      result.index = classes_.at(frontend::NonNullableTypeName(type));
      break;
    case Source::Interface:
      result.kind = Kind::Interface;
      result.index = model_->interfaces.at(frontend::NonNullableTypeName(type)).index;
      break;
    case Source::Array:
      result.kind = Kind::Array;
      result.index = Array(type);
      break;
    case Source::Set:
    case Source::Map:
      result.kind = Kind::Collection;
      result.index = Collection(type);
      break;
    case Source::Function:
      result.kind = Kind::Callable;
      result.index = Signature(type);
      break;
    case Source::Future:
      result.kind = Kind::Future;
      result.index = Signature(type);
      break;
    case Source::Completer:
      result.kind = Kind::Completer;
      result.index = Signature(type);
      break;
    case Source::Timer:
      result.kind = Kind::Timer;
      break;
    case Source::CancellationToken:
      result.kind = Kind::CancellationToken;
      break;
    case Source::Invalid:
    case Source::Null:
    case Source::Void:
    case Source::Parameter:
      std::unreachable();
  }
  return result;
}

std::uint32_t LayoutDescription::Array(const SourceType& type) {
  const auto key = frontend::NonNullableTypeName(type);
  if (const auto found = arrays_.find(key); found != arrays_.end()) {
    return found->second;
  }
  const auto element = Type(model_->array_elements.at(key));
  const auto index = static_cast<std::uint32_t>(result_.arrays.size());
  result_.arrays.push_back({.name = std::string(key), .element = element});
  arrays_.emplace(key, index);
  return index;
}

std::uint32_t LayoutDescription::Collection(const SourceType& type) {
  const auto key = frontend::NonNullableTypeName(type);
  if (const auto found = collections_.find(key); found != collections_.end()) {
    return found->second;
  }
  const auto& arguments = model_->generic_arguments.at(key);
  layout::Collection descriptor{.name = std::string(key), .key = Type(arguments.front())};
  if (type.kind == SourceType::Kind::Map) {
    descriptor.value = Type(arguments.back());
  }
  const auto index = static_cast<std::uint32_t>(result_.collections.size());
  result_.collections.push_back(std::move(descriptor));
  collections_.emplace(key, index);
  return index;
}

std::uint32_t LayoutDescription::Signature(const SourceType& type) {
  const auto key = frontend::NonNullableTypeName(type);
  if (const auto found = signatures_.find(key); found != signatures_.end()) {
    return found->second;
  }
  layout::Signature descriptor{.name = std::string(key), .parameters = {}};
  if (type.kind == SourceType::Kind::Function) {
    const auto& function = model_->function_types.at(key);
    for (const auto& parameter : function.parameters) {
      descriptor.parameters.push_back({.type = Type(parameter.type)});
    }
    if (function.return_type.kind != SourceType::Kind::Void) {
      descriptor.result = Type(function.return_type);
    }
  } else {
    const auto& arguments = model_->generic_arguments.at(key);
    const auto& payload = arguments.front();
    if (payload.kind != SourceType::Kind::Void) {
      descriptor.result = Type(payload);
    }
  }
  const auto index = static_cast<std::uint32_t>(result_.signatures.size());
  result_.signatures.push_back(std::move(descriptor));
  signatures_.emplace(key, index);
  return index;
}

std::uint32_t LayoutDescription::CallableSignature(const Class& type) {
  const auto& target = type.callable;
  if (!target) {
    std::unreachable();
  }
  const auto& callable = *target;
  const auto& program = *program_;
  const auto& signature = callable.mode == CallableMode::InterfaceMethod
                              ? type.callable_type
                              : program.functions.at(callable.function).signature;
  if (!signature) {
    std::unreachable();
  }
  const auto& call = *signature;
  // 绑定方法的接收者不属于宿主可见参数。其余参数标志直接沿用 lowering 的事实。
  const auto skipped =
      callable.mode == CallableMode::BoundMethod && call.hidden_parameters == 0 ? 1U : 0U;
  layout::Signature descriptor{.name = {}, .parameters = {}};
  for (std::size_t index = skipped; index < call.parameters.size(); ++index) {
    const auto& parameter = call.parameters.at(index);
    descriptor.parameters.push_back({
        .type = Type(program.runtime_types.at(parameter.type)),
        .optional = parameter.optional,
        .rest = parameter.rest,
    });
  }
  if (call.result) {
    descriptor.result = Type(program.runtime_types.at(*call.result));
  }
  const auto index = static_cast<std::uint32_t>(result_.signatures.size());
  result_.signatures.push_back(std::move(descriptor));
  return index;
}

layout::Class LayoutDescription::DescribeClass(const Class& type) {
  layout::Class descriptor{
      .name = type.name,
      .native_key = type.native_key,
      .native_contract = type.native_contract,
      .field_count = static_cast<std::uint32_t>(type.field_types.size()),
      .reference_fields = {},
      .callable = type.callable,
      .interfaces = type.interfaces,
      .dispatch_targets = type.dispatch_targets,
  };
  switch (type.kind) {
    case Class::Kind::Instance:
    case Class::Kind::CaptureCell:
      break;
    case Class::Kind::RuntimeError:
      descriptor.kind = layout::Class::Kind::RuntimeError;
      break;
    case Class::Kind::DateParts:
      descriptor.kind = layout::Class::Kind::DateParts;
      break;
    case Class::Kind::Enum:
      descriptor.kind = layout::Class::Kind::Enum;
      break;
    case Class::Kind::Host:
      descriptor.kind = layout::Class::Kind::Host;
      break;
    case Class::Kind::Metadata:
      descriptor.kind = layout::Class::Kind::Metadata;
      break;
    case Class::Kind::ReflectionIdentity:
      descriptor.kind = layout::Class::Kind::ReflectionIdentity;
      break;
  }
  for (std::size_t index = 0; index < type.field_types.size(); ++index) {
    if (HasReference(type.field_types.at(index).kind)) {
      descriptor.reference_fields.push_back(static_cast<std::uint32_t>(index));
    }
  }
  if (type.callable) {
    descriptor.callable_type = CallableSignature(type);
  }
  return descriptor;
}

layout::Program LayoutDescription::Build() {
  const auto& program = *program_;
  const auto collect_exports = [&](const auto& declarations) {
    for (const auto& declaration : declarations) {
      exported_.emplace(declaration.name,
                        declaration.export_kind == frontend::ExportKind::Exported);
    }
  };
  collect_exports(source_->classes);
  collect_exports(source_->enums);
  collect_exports(source_->interfaces);
  for (const auto& [name, descriptor] : model_->interfaces) {
    // 模板定义检查的抽象应用没有运行时编号，不能覆盖具体接口的布局身份。
    if (descriptor.type.is_abstract) {
      continue;
    }
    if (result_.interfaces.size() <= descriptor.index) {
      result_.interfaces.resize(static_cast<std::size_t>(descriptor.index) + 1);
    }
    result_.interfaces.at(descriptor.index) = name;
  }
  for (std::size_t index = 0; index < program.classes.size(); ++index) {
    classes_.emplace(program.classes.at(index).name, static_cast<std::uint32_t>(index));
  }
  for (const auto& type : program.classes) {
    result_.classes.push_back(DescribeClass(type));
  }
  for (const auto& type : program.array_types) {
    result_.array_types.push_back(Array(type));
  }
  for (const auto& type : program.collection_types) {
    result_.collection_types.push_back(Collection(type));
  }
  for (const auto& type : program.runtime_types) {
    result_.types.push_back(Type(type));
  }
  for (std::size_t index = 0; index < program.classes.size(); ++index) {
    Canonicalize(
        {.kind = layout::Type::Kind::Instance, .index = static_cast<std::uint32_t>(index)});
  }
  for (std::size_t index = 0; index < result_.interfaces.size(); ++index) {
    if (!result_.interfaces.at(index).empty()) {
      Canonicalize(
          {.kind = layout::Type::Kind::Interface, .index = static_cast<std::uint32_t>(index)});
    }
  }
  return std::move(result_);
}

}  // namespace

layout::Program DescribeLayouts(const Program& program,
                                const frontend::SemanticModel& model,
                                const frontend::Program& source) {
  return LayoutDescription(program, model, source).Build();
}

}  // namespace vela::ir
