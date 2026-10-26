#include "compiler/runtime_layouts.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "compiler/frontend/type_checker.h"
#include "compiler/ir/ir.h"
#include "runtime/isolate.h"
#include "runtime/object.h"
#include "runtime/value.h"

namespace vela::compiler {

std::vector<runtime::ClassLayout*> CreateClassLayouts(const ir::Program& program,
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
          kind == frontend::TypeDescriptor::Kind::ByteData) {
        reference_fields.push_back(
            class_info.kind == ir::Class::Kind::Expected ? 0U : static_cast<std::uint32_t>(index));
      }
    }
    runtime::ClassLayout* layout =
        isolate.CreateClassLayout(class_info.kind == ir::Class::Kind::Expected
                                      ? 1U
                                      : static_cast<std::uint32_t>(class_info.field_types.size()),
                                  reference_fields,
                                  {
                                      .callable = class_info.callable,
                                      .interfaces = class_info.interfaces,
                                      .dispatch_targets = class_info.dispatch_targets,
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
                runtime::Isolate& isolate,
                LayoutCache& cache)
      : program_(program), model_(model), isolate_(isolate), cache_(cache) {
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
  const runtime::ArrayLayout* Array(const frontend::TypeDescriptor& type);
  const runtime::CollectionLayout* Collection(const frontend::TypeDescriptor& type);

  const ir::Program& program_;
  const frontend::SemanticModel& model_;
  runtime::Isolate& isolate_;
  absl::flat_hash_map<std::string, runtime::ClassLayout*> classes_;
  LayoutCache& cache_;
};

runtime::ArrayLayout LayoutBuilder::Element(const frontend::TypeDescriptor& type) {
  runtime::ArrayLayout layout;
  layout.nullable_elements = type.nullable;
  using Kind = frontend::TypeDescriptor::Kind;
  switch (type.kind) {
    case Kind::Bool:
      layout.element_kind = runtime::ValueKind::Boolean;
      break;
    case Kind::Int:
    case Kind::Enum:
      layout.element_kind = runtime::ValueKind::Integer;
      break;
    case Kind::Double:
      layout.element_kind = runtime::ValueKind::Double;
      break;
    case Kind::String:
      layout.element_kind = runtime::ValueKind::String;
      break;
    case Kind::MutableString:
      layout.element_kind = runtime::ValueKind::Object;
      layout.element_object_kind = runtime::ObjectKind::MutableString;
      break;
    case Kind::ByteArray:
      layout.element_kind = runtime::ValueKind::Object;
      layout.element_object_kind = runtime::ObjectKind::ByteArray;
      break;
    case Kind::ByteData:
      layout.element_kind = runtime::ValueKind::Object;
      layout.element_object_kind = runtime::ObjectKind::ByteData;
      break;
    case Kind::Class:
    case Kind::Expected:
      layout.element_kind = runtime::ValueKind::Object;
      layout.element_class = classes_.at(TypeKey(type));
      if (type.kind == Kind::Expected) {
        layout.element_object_kind = runtime::ObjectKind::Expected;
      }
      break;
    case Kind::Future:
      layout.element_kind = runtime::ValueKind::Object;
      layout.element_object_kind = runtime::ObjectKind::Future;
      break;
    case Kind::Completer:
      layout.element_kind = runtime::ValueKind::Object;
      layout.element_object_kind = runtime::ObjectKind::Completer;
      break;
    case Kind::Timer:
      layout.element_kind = runtime::ValueKind::Object;
      layout.element_object_kind = runtime::ObjectKind::Timer;
      break;
    case Kind::CancellationToken:
      layout.element_kind = runtime::ValueKind::Object;
      layout.element_object_kind = runtime::ObjectKind::CancellationToken;
      break;
    case Kind::Interface:
      layout.element_kind = runtime::ValueKind::Object;
      layout.element_interface = model_.interfaces.at(TypeKey(type)).index;
      break;
    case Kind::Function:
      layout.element_kind = runtime::ValueKind::Object;
      layout.callable_elements = true;
      break;
    case Kind::Array:
      layout.element_kind = runtime::ValueKind::Object;
      layout.element_object_kind = runtime::ObjectKind::Array;
      layout.element_array = Array(type);
      break;
    case Kind::Set:
    case Kind::Map:
      layout.element_kind = runtime::ValueKind::Object;
      layout.element_object_kind = runtime::ObjectKind::Collection;
      layout.element_collection = Collection(type);
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

RuntimeLayouts LayoutBuilder::Build() {
  RuntimeLayouts result;
  for (const auto& type : program_.array_types) {
    result.arrays.push_back(Array(type));
  }
  for (const auto& type : program_.collection_types) {
    result.collections.push_back(Collection(type));
  }
  return result;
}

}  // namespace

RuntimeLayouts CreateRuntimeLayouts(const ir::Program& program,
                                    const frontend::SemanticModel& model,
                                    std::span<runtime::ClassLayout* const> classes,
                                    runtime::Isolate& isolate,
                                    LayoutCache* cache) {
  LayoutCache local;
  return LayoutBuilder(program, model, classes, isolate, cache == nullptr ? local : *cache).Build();
}

}  // namespace vela::compiler
