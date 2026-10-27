#include "compiler/runtime_layouts.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "base/callable.h"
#include "compiler/frontend/type_checker.h"
#include "compiler/ir/ir.h"
#include "runtime/isolate.h"
#include "runtime/object.h"
#include "runtime/value.h"

namespace vela::compiler {

std::vector<const runtime::InterfaceType*> CreateInterfaceTypes(
    const frontend::SemanticModel& model,
    runtime::Isolate& isolate,
    LayoutCache* cache) {
  std::vector<const runtime::InterfaceType*> interfaces;
  for (const auto& [name, descriptor] : model.interfaces) {
    if (interfaces.size() <= descriptor.index) {
      interfaces.resize(static_cast<std::size_t>(descriptor.index) + 1);
    }
    const runtime::InterfaceType* type = nullptr;
    if (cache != nullptr) {
      const auto found = cache->interfaces.find(name);
      if (found != cache->interfaces.end()) {
        type = found->second;
      }
    }
    if (type == nullptr) {
      type = isolate.CreateInterfaceType();
      if (cache != nullptr) {
        cache->interfaces.emplace(name, type);
      }
    }
    interfaces.at(descriptor.index) = type;
  }
  return interfaces;
}

namespace {

std::vector<std::uint32_t> ReferenceFields(const ir::Class& class_info) {
  std::vector<std::uint32_t> reference_fields;
  for (std::size_t index = 0; index < class_info.field_types.size(); ++index) {
    const auto kind = class_info.field_types.at(index).kind;
    if (kind == frontend::TypeDescriptor::Kind::Set ||
        kind == frontend::TypeDescriptor::Kind::Map ||
        kind == frontend::TypeDescriptor::Kind::Expected ||
        kind == frontend::TypeDescriptor::Kind::Future ||
        kind == frontend::TypeDescriptor::Kind::Completer ||
        kind == frontend::TypeDescriptor::Kind::Timer ||
        kind == frontend::TypeDescriptor::Kind::CancellationToken ||
        kind == frontend::TypeDescriptor::Kind::Interface ||
        kind == frontend::TypeDescriptor::Kind::Function ||
        kind == frontend::TypeDescriptor::Kind::Class ||
        kind == frontend::TypeDescriptor::Kind::Array ||
        kind == frontend::TypeDescriptor::Kind::String ||
        kind == frontend::TypeDescriptor::Kind::MutableString ||
        kind == frontend::TypeDescriptor::Kind::ByteArray ||
        kind == frontend::TypeDescriptor::Kind::ByteData ||
        kind == frontend::TypeDescriptor::Kind::RegExp ||
        kind == frontend::TypeDescriptor::Kind::RegExpMatch ||
        kind == frontend::TypeDescriptor::Kind::Dynamic) {
      reference_fields.push_back(
          class_info.kind == ir::Class::Kind::Expected ? 0U : static_cast<std::uint32_t>(index));
    }
  }
  return reference_fields;
}

}  // namespace

std::vector<runtime::ClassLayout*> CreateClassLayouts(
    const ir::Program& program,
    std::span<const runtime::InterfaceType* const> interface_types,
    runtime::Isolate& isolate,
    LayoutCache* cache) {
  std::vector<runtime::ClassLayout*> class_layouts;
  class_layouts.reserve(program.classes.size());
  for (const ir::Class& class_info : program.classes) {
    if (cache != nullptr) {
      const auto found = cache->classes.find(class_info.name);
      if (found != cache->classes.end()) {
        class_layouts.push_back(found->second);
        continue;
      }
    }
    if (class_info.kind == ir::Class::Kind::RuntimeError) {
      class_layouts.push_back(&isolate.error_layout());
      continue;
    }
    if (class_info.kind == ir::Class::Kind::DateParts) {
      class_layouts.push_back(&isolate.date_parts_layout());
      continue;
    }
    const auto reference_fields = ReferenceFields(class_info);
    std::vector<const runtime::InterfaceType*> interfaces;
    interfaces.reserve(class_info.interfaces.size());
    for (const auto index : class_info.interfaces) {
      interfaces.push_back(interface_types.subspan(index).front());
    }
    runtime::ClassLayout* layout =
        isolate.CreateClassLayout(class_info.kind == ir::Class::Kind::Expected
                                      ? 1U
                                      : static_cast<std::uint32_t>(class_info.field_types.size()),
                                  reference_fields,
                                  {
                                      .callable = class_info.callable,
                                      .interfaces = std::move(interfaces),
                                      .dispatch_targets = class_info.dispatch_targets,
                                      .is_enum = class_info.kind == ir::Class::Kind::Enum,
                                  });
    class_layouts.push_back(layout);
    if (cache != nullptr) {
      cache->classes.emplace(class_info.name, layout);
    }
  }
  return class_layouts;
}

namespace {

std::string TypeKey(const frontend::TypeDescriptor& type) {
  return type.nullable ? type.name.substr(0, type.name.size() - 1) : type.name;
}

class LayoutBuilder final {
 public:
  LayoutBuilder(const ir::Program& program,
                const frontend::SemanticModel& model,
                std::span<runtime::ClassLayout* const> classes,
                std::span<const runtime::InterfaceType* const> interfaces,
                runtime::Isolate& isolate,
                LayoutCache& cache)
      : program_(program),
        model_(model),
        isolate_(isolate),
        cache_(cache),
        interfaces_(interfaces) {
    for (std::size_t index = 0; index < program.classes.size(); ++index) {
      classes_.emplace(program.classes.at(index).name, classes.subspan(index).front());
    }
  }
  ~LayoutBuilder() = default;
  LayoutBuilder(const LayoutBuilder&) = delete;
  LayoutBuilder& operator=(const LayoutBuilder&) = delete;
  LayoutBuilder(LayoutBuilder&&) = delete;
  LayoutBuilder& operator=(LayoutBuilder&&) = delete;

  RuntimeLayouts Build();

 private:
  runtime::ArrayLayout Element(const frontend::TypeDescriptor& type);
  runtime::TypeConstraint Constraint(const frontend::TypeDescriptor& type);
  const runtime::TypeSignature* Signature(const frontend::TypeDescriptor& type);
  void BindCallableTypes();
  const runtime::ArrayLayout* Array(const frontend::TypeDescriptor& type);
  const runtime::CollectionLayout* Collection(const frontend::TypeDescriptor& type);

  const ir::Program& program_;
  const frontend::SemanticModel& model_;
  runtime::Isolate& isolate_;
  absl::flat_hash_map<std::string, runtime::ClassLayout*> classes_;
  LayoutCache& cache_;
  std::span<const runtime::InterfaceType* const> interfaces_;
};

runtime::ArrayLayout LayoutBuilder::Element(const frontend::TypeDescriptor& type) {
  runtime::ArrayLayout layout;
  layout.element.nullable = type.nullable;
  using Kind = frontend::TypeDescriptor::Kind;
  switch (type.kind) {
    case Kind::Dynamic:
      layout.element = {.dynamic = true};
      break;
    case Kind::Bool:
      layout.element.kind = runtime::ValueKind::Boolean;
      break;
    case Kind::Int:
      layout.element.kind = runtime::ValueKind::Integer;
      break;
    case Kind::Enum:
      layout.element.kind = runtime::ValueKind::Integer;
      layout.element.instance = classes_.at(TypeKey(type));
      break;
    case Kind::Double:
      layout.element.kind = runtime::ValueKind::Double;
      break;
    case Kind::String:
      layout.element.kind = runtime::ValueKind::String;
      break;
    case Kind::MutableString:
      layout.element.kind = runtime::ValueKind::Object;
      layout.element.object_kind = runtime::ObjectKind::MutableString;
      break;
    case Kind::ByteArray:
      layout.element.kind = runtime::ValueKind::Object;
      layout.element.object_kind = runtime::ObjectKind::ByteArray;
      break;
    case Kind::ByteData:
      layout.element.kind = runtime::ValueKind::Object;
      layout.element.object_kind = runtime::ObjectKind::ByteData;
      break;
    case Kind::RegExp:
      layout.element.kind = runtime::ValueKind::Object;
      layout.element.object_kind = runtime::ObjectKind::RegExp;
      break;
    case Kind::RegExpMatch:
      layout.element.kind = runtime::ValueKind::Object;
      layout.element.object_kind = runtime::ObjectKind::RegExpMatch;
      break;
    case Kind::Class:
      layout.element.kind = runtime::ValueKind::Object;
      layout.element.instance = classes_.at(TypeKey(type));
      break;
    case Kind::Expected:
      layout.element.kind = runtime::ValueKind::Object;
      layout.element.object_kind = runtime::ObjectKind::Expected;
      layout.element.signature = Signature(type);
      break;
    case Kind::Future:
      layout.element.kind = runtime::ValueKind::Object;
      layout.element.object_kind = runtime::ObjectKind::Future;
      layout.element.signature = Signature(type);
      break;
    case Kind::Completer:
      layout.element.kind = runtime::ValueKind::Object;
      layout.element.object_kind = runtime::ObjectKind::Completer;
      layout.element.signature = Signature(type);
      break;
    case Kind::Timer:
      layout.element.kind = runtime::ValueKind::Object;
      layout.element.object_kind = runtime::ObjectKind::Timer;
      break;
    case Kind::CancellationToken:
      layout.element.kind = runtime::ValueKind::Object;
      layout.element.object_kind = runtime::ObjectKind::CancellationToken;
      break;
    case Kind::Interface:
      layout.element.kind = runtime::ValueKind::Object;
      layout.element.interface =
          interfaces_.subspan(model_.interfaces.at(TypeKey(type)).index).front();
      break;
    case Kind::Function:
      layout.element.kind = runtime::ValueKind::Object;
      layout.element.callable = true;
      layout.element.signature = Signature(type);
      break;
    case Kind::Array:
      layout.element.kind = runtime::ValueKind::Object;
      layout.element.object_kind = runtime::ObjectKind::Array;
      layout.element.array = Array(type);
      break;
    case Kind::Set:
    case Kind::Map:
      layout.element.kind = runtime::ValueKind::Object;
      layout.element.object_kind = runtime::ObjectKind::Collection;
      layout.element.collection = Collection(type);
      break;
    case Kind::Invalid:
    case Kind::Null:
    case Kind::Void:
      std::unreachable();
  }
  return layout;
}

const runtime::ArrayLayout* LayoutBuilder::Array(const frontend::TypeDescriptor& type) {
  const auto key = TypeKey(type);
  if (const auto found = cache_.arrays.find(key); found != cache_.arrays.end()) {
    return found->second;
  }
  const auto layout = Element(model_.array_elements.at(key));
  const auto* stored = isolate_.CreateArrayLayout(layout);
  cache_.arrays.emplace(key, stored);
  return stored;
}

const runtime::CollectionLayout* LayoutBuilder::Collection(const frontend::TypeDescriptor& type) {
  const auto key = TypeKey(type);
  if (const auto found = cache_.collections.find(key); found != cache_.collections.end()) {
    return found->second;
  }
  const auto& arguments = model_.generic_arguments.at(key);
  runtime::CollectionLayout layout;
  layout.keys = Element(arguments.front());
  if (type.kind == frontend::TypeDescriptor::Kind::Map) {
    layout.values = Element(arguments.back());
  }
  const auto* stored = isolate_.CreateCollectionLayout(layout);
  cache_.collections.emplace(key, stored);
  return stored;
}

runtime::TypeConstraint LayoutBuilder::Constraint(const frontend::TypeDescriptor& type) {
  auto result = Element(type).element;
  if (type.kind == frontend::TypeDescriptor::Kind::Enum) {
    result.kind = runtime::ValueKind::Object;
  }
  return result;
}

const runtime::TypeSignature* LayoutBuilder::Signature(const frontend::TypeDescriptor& type) {
  const auto key = type.kind == frontend::TypeDescriptor::Kind::Function && type.nullable
                       ? type.name.substr(1, type.name.size() - 3)
                       : TypeKey(type);
  if (const auto found = cache_.signatures.find(key); found != cache_.signatures.end()) {
    return found->second;
  }
  runtime::TypeSignature signature;
  if (type.kind == frontend::TypeDescriptor::Kind::Function) {
    const auto& function = model_.function_types.at(key);
    for (const auto& parameter : function.parameters) {
      signature.parameters.push_back({.type = Constraint(parameter.type)});
    }
    if (function.return_type.kind != frontend::TypeDescriptor::Kind::Void) {
      signature.result = Constraint(function.return_type);
    }
  } else {
    const auto& arguments = model_.generic_arguments.at(key);
    const auto& payload = arguments.front();
    if (payload.kind != frontend::TypeDescriptor::Kind::Void) {
      signature.result = Constraint(payload);
    }
    if (type.kind == frontend::TypeDescriptor::Kind::Expected) {
      signature.parameters.push_back({.type = Constraint(arguments.back())});
    }
  }
  const auto* stored = isolate_.CreateTypeSignature(std::move(signature));
  cache_.signatures.emplace(key, stored);
  return stored;
}

void LayoutBuilder::BindCallableTypes() {
  for (const auto& type : program_.classes) {
    if (type.kind == ir::Class::Kind::Expected) {
      classes_.at(type.name)->BindExpectedType(
          *Signature({.kind = frontend::TypeDescriptor::Kind::Expected, .name = type.name}));
    }
    if (!type.callable || type.callable_type.empty()) {
      continue;
    }
    const runtime::TypeSignature* signature = nullptr;
    if (type.callable->mode == CallableMode::InterfaceMethod) {
      signature =
          Signature({.kind = frontend::TypeDescriptor::Kind::Function, .name = type.callable_type});
    } else {
      const auto& function = program_.functions.at(type.callable->function);
      if (!function.signature) {
        std::unreachable();
      }
      const auto& call = *function.signature;
      runtime::TypeSignature descriptor;
      const auto skipped =
          type.callable->mode == CallableMode::BoundMethod && call.hidden_parameters == 0 ? 1U : 0U;
      for (std::size_t index = skipped; index < call.parameters.size(); ++index) {
        const auto& parameter = call.parameters.at(index);
        descriptor.parameters.push_back({
            .type = Constraint(program_.runtime_types.at(parameter.type)),
            .optional = parameter.optional,
            .rest = parameter.rest,
        });
      }
      if (call.result) {
        descriptor.result = Constraint(program_.runtime_types.at(*call.result));
      }
      signature = isolate_.CreateTypeSignature(std::move(descriptor));
    }
    classes_.at(type.name)->BindCallableType(*signature);
  }
}

RuntimeLayouts LayoutBuilder::Build() {
  RuntimeLayouts result;
  for (const auto& type : program_.array_types) {
    result.arrays.push_back(Array(type));
  }
  for (const auto& type : program_.collection_types) {
    result.collections.push_back(Collection(type));
  }
  for (const auto& type : program_.runtime_types) {
    result.types.push_back(Constraint(type));
  }
  BindCallableTypes();
  return result;
}

}  // namespace

RuntimeLayouts CreateRuntimeLayouts(const ir::Program& program,
                                    const frontend::SemanticModel& model,
                                    std::span<runtime::ClassLayout* const> classes,
                                    std::span<const runtime::InterfaceType* const> interfaces,
                                    runtime::Isolate& isolate,
                                    LayoutCache* cache) {
  LayoutCache local;
  return LayoutBuilder(program, model, classes, interfaces, isolate,
                       cache == nullptr ? local : *cache)
      .Build();
}

}  // namespace vela::compiler
