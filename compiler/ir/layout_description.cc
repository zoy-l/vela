#include "compiler/ir/layout_description.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include <absl/container/flat_hash_map.h>

#include "base/callable.h"
#include "base/layout_description.h"
#include "compiler/frontend/typing/type_checker.h"
#include "compiler/ir/ir.h"

namespace vela::ir {
namespace {

using SourceType = frontend::TypeDescriptor;

std::string TypeKey(const SourceType& type) {
  if (!type.nullable) {
    return std::string(type.name);
  }
  return type.kind == SourceType::Kind::Function
             ? std::string(type.name.substr(1, type.name.size() - 3))
             : std::string(type.name.substr(0, type.name.size() - 1));
}

bool HasReference(SourceType::Kind kind) {
  using Kind = SourceType::Kind;
  return kind != Kind::Invalid && kind != Kind::Null && kind != Kind::Void && kind != Kind::Bool &&
         kind != Kind::Int && kind != Kind::Double && kind != Kind::Enum;
}

class LayoutDescription final {
 public:
  LayoutDescription(const Program& program, const frontend::SemanticModel& model)
      : program_(&program), model_(&model) {}

  layout::Program Build();

 private:
  layout::Type Type(const SourceType& type);
  std::uint32_t Array(const SourceType& type);
  std::uint32_t Collection(const SourceType& type);
  std::uint32_t Signature(const SourceType& type);
  std::uint32_t CallableSignature(const Class& type);
  layout::Class DescribeClass(const Class& type);

  const Program* program_;
  const frontend::SemanticModel* model_;
  layout::Program result_;
  absl::flat_hash_map<std::string, std::uint32_t> classes_;
  absl::flat_hash_map<std::string, std::uint32_t> arrays_;
  absl::flat_hash_map<std::string, std::uint32_t> collections_;
  absl::flat_hash_map<std::string, std::uint32_t> signatures_;
};

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
      result.index = classes_.at(TypeKey(type));
      break;
    case Source::Interface:
      result.kind = Kind::Interface;
      result.index = model_->interfaces.at(TypeKey(type)).index;
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
  const auto key = TypeKey(type);
  if (const auto found = arrays_.find(key); found != arrays_.end()) {
    return found->second;
  }
  const auto element = Type(model_->array_elements.at(key));
  const auto index = static_cast<std::uint32_t>(result_.arrays.size());
  result_.arrays.push_back({.name = key, .element = element});
  arrays_.emplace(key, index);
  return index;
}

std::uint32_t LayoutDescription::Collection(const SourceType& type) {
  const auto key = TypeKey(type);
  if (const auto found = collections_.find(key); found != collections_.end()) {
    return found->second;
  }
  const auto& arguments = model_->generic_arguments.at(key);
  layout::Collection descriptor{.name = key, .key = Type(arguments.front())};
  if (type.kind == SourceType::Kind::Map) {
    descriptor.value = Type(arguments.back());
  }
  const auto index = static_cast<std::uint32_t>(result_.collections.size());
  result_.collections.push_back(std::move(descriptor));
  collections_.emplace(key, index);
  return index;
}

std::uint32_t LayoutDescription::Signature(const SourceType& type) {
  const auto key = TypeKey(type);
  if (const auto found = signatures_.find(key); found != signatures_.end()) {
    return found->second;
  }
  layout::Signature descriptor{.name = key, .parameters = {}};
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
  if (callable.mode == CallableMode::InterfaceMethod) {
    return Signature({.kind = SourceType::Kind::Function, .name = type.callable_type});
  }
  const auto& program = *program_;
  const auto& signature = program.functions.at(callable.function).signature;
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
  }
  for (std::size_t index = 0; index < type.field_types.size(); ++index) {
    if (HasReference(type.field_types.at(index).kind)) {
      descriptor.reference_fields.push_back(static_cast<std::uint32_t>(index));
    }
  }
  if (type.callable && !type.callable_type.empty()) {
    descriptor.callable_type = CallableSignature(type);
  }
  return descriptor;
}

layout::Program LayoutDescription::Build() {
  const auto& program = *program_;
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
  return std::move(result_);
}

}  // namespace

layout::Program DescribeLayouts(const Program& program, const frontend::SemanticModel& model) {
  return LayoutDescription(program, model).Build();
}

}  // namespace vela::ir
