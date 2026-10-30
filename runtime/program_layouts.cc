#include "runtime/program_layouts.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "base/layout_description.h"
#include "runtime/isolate.h"
#include "runtime/object.h"
#include "runtime/value.h"

namespace vela::runtime {
namespace {

class LayoutInstantiation final {
 public:
  LayoutInstantiation(const layout::Program& description,
                      Isolate& isolate,
                      LayoutCache& cache,
                      std::span<const ClassLayoutOptions> classes)
      : description_(&description),
        isolate_(&isolate),
        cache_(&cache),
        class_options_(classes),
        arrays_(description.arrays.size()),
        collections_(description.collections.size()),
        signatures_(description.signatures.size()) {}

  ProgramLayouts Build();

 private:
  TypeConstraint Constraint(const layout::Type& type);
  ArrayLayout Element(const layout::Type& type);
  const ArrayLayout* Array(std::uint32_t index);
  const CollectionLayout* Collection(std::uint32_t index);
  const TypeSignature* Signature(std::uint32_t index);
  ClassLayout* Class(std::size_t index);

  const layout::Program* description_;
  Isolate* isolate_;
  LayoutCache* cache_;
  std::span<const ClassLayoutOptions> class_options_;
  std::vector<const InterfaceType*> interfaces_;
  std::vector<ClassLayout*> classes_;
  std::vector<const ArrayLayout*> arrays_;
  std::vector<const CollectionLayout*> collections_;
  std::vector<const TypeSignature*> signatures_;
};

TypeConstraint LayoutInstantiation::Constraint(const layout::Type& type) {
  TypeConstraint result{.kind = ValueKind::Object, .nullable = type.nullable};
  using Kind = layout::Type::Kind;
  switch (type.kind) {
    case Kind::Dynamic:
      return {.dynamic = true};
    case Kind::Boolean:
      result.kind = ValueKind::Boolean;
      break;
    case Kind::Integer:
      result.kind = ValueKind::Integer;
      break;
    case Kind::Double:
      result.kind = ValueKind::Double;
      break;
    case Kind::String:
      result.kind = ValueKind::String;
      break;
    case Kind::MutableString:
      result.object_kind = ObjectKind::MutableString;
      break;
    case Kind::ByteArray:
      result.object_kind = ObjectKind::ByteArray;
      break;
    case Kind::ByteData:
      result.object_kind = ObjectKind::ByteData;
      break;
    case Kind::RegExp:
      result.object_kind = ObjectKind::RegExp;
      break;
    case Kind::RegExpMatch:
      result.object_kind = ObjectKind::RegExpMatch;
      break;
    case Kind::Instance:
    case Kind::Enum:
      result.nominal = &classes_.at(type.index)->nominal_type();
      break;
    case Kind::Interface:
      result.interface = interfaces_.at(type.index);
      break;
    case Kind::Array:
      result.object_kind = ObjectKind::Array;
      result.array = Array(type.index);
      break;
    case Kind::Collection:
      result.object_kind = ObjectKind::Collection;
      result.collection = Collection(type.index);
      break;
    case Kind::Callable:
      result.callable = true;
      result.signature = Signature(type.index);
      break;
    case Kind::Expected:
      result.object_kind = ObjectKind::Expected;
      result.signature = Signature(type.index);
      break;
    case Kind::Future:
      result.object_kind = ObjectKind::Future;
      result.signature = Signature(type.index);
      break;
    case Kind::Completer:
      result.object_kind = ObjectKind::Completer;
      result.signature = Signature(type.index);
      break;
    case Kind::Timer:
      result.object_kind = ObjectKind::Timer;
      break;
    case Kind::CancellationToken:
      result.object_kind = ObjectKind::CancellationToken;
      break;
  }
  return result;
}

ArrayLayout LayoutInstantiation::Element(const layout::Type& type) {
  auto element = Constraint(type);
  // 枚举在连续容器中存 ordinal，在动态值和函数签名中保留装箱后的名义身份。
  if (type.kind == layout::Type::Kind::Enum) {
    element.kind = ValueKind::Integer;
  }
  return {.element = element};
}

const ArrayLayout* LayoutInstantiation::Array(std::uint32_t index) {
  auto*& stored = arrays_.at(index);
  if (stored != nullptr) {
    return stored;
  }
  const auto& description = description_->arrays.at(index);
  if (const auto found = cache_->arrays.find(description.name); found != cache_->arrays.end()) {
    stored = found->second;
  } else {
    stored = isolate_->CreateArrayLayout(Element(description.element));
    cache_->arrays.emplace(description.name, stored);
  }
  return stored;
}

const CollectionLayout* LayoutInstantiation::Collection(std::uint32_t index) {
  auto*& stored = collections_.at(index);
  if (stored != nullptr) {
    return stored;
  }
  const auto& description = description_->collections.at(index);
  if (const auto found = cache_->collections.find(description.name);
      found != cache_->collections.end()) {
    stored = found->second;
  } else {
    CollectionLayout result{.keys = Element(description.key)};
    if (description.value) {
      result.values = Element(*description.value);
    }
    stored = isolate_->CreateCollectionLayout(result);
    cache_->collections.emplace(description.name, stored);
  }
  return stored;
}

const TypeSignature* LayoutInstantiation::Signature(std::uint32_t index) {
  auto*& stored = signatures_.at(index);
  if (stored != nullptr) {
    return stored;
  }
  const auto& description = description_->signatures.at(index);
  if (!description.name.empty()) {
    if (const auto found = cache_->signatures.find(description.name);
        found != cache_->signatures.end()) {
      stored = found->second;
      return stored;
    }
  }
  TypeSignature result;
  for (const auto& parameter : description.parameters) {
    result.parameters.push_back({
        .type = Constraint(parameter.type),
        .optional = parameter.optional,
        .rest = parameter.rest,
    });
  }
  if (description.result) {
    result.result = Constraint(*description.result);
  }
  stored = isolate_->CreateTypeSignature(std::move(result));
  if (!description.name.empty()) {
    cache_->signatures.emplace(description.name, stored);
  }
  return stored;
}

ClassLayout* LayoutInstantiation::Class(std::size_t index) {
  const auto& description = description_->classes.at(index);
  if (const auto found = cache_->classes.find(description.name); found != cache_->classes.end()) {
    return found->second;
  }
  if (description.kind == layout::Class::Kind::RuntimeError) {
    return &isolate_->error_layout();
  }
  if (description.kind == layout::Class::Kind::DateParts) {
    return &isolate_->date_parts_layout();
  }
  std::vector<const InterfaceType*> interfaces;
  interfaces.reserve(description.interfaces.size());
  for (const auto interface : description.interfaces) {
    interfaces.push_back(interfaces_.at(interface));
  }
  auto options =
      class_options_.empty() ? ClassLayoutOptions{} : class_options_.subspan(index).front();
  options.callable = description.callable;
  options.interfaces = std::move(interfaces);
  options.dispatch_targets = description.dispatch_targets;
  options.is_enum = description.kind == layout::Class::Kind::Enum;
  auto* stored = isolate_->CreateClassLayout(description.field_count, description.reference_fields,
                                             std::move(options));
  cache_->classes.emplace(description.name, stored);
  return stored;
}

ProgramLayouts LayoutInstantiation::Build() {
  interfaces_.reserve(description_->interfaces.size());
  classes_.reserve(description_->classes.size());
  for (const auto& name : description_->interfaces) {
    // 增量会话的接口编号可能有空洞；空槽不创建新的名义身份。
    if (name.empty()) {
      interfaces_.push_back(nullptr);
      continue;
    }
    const auto [entry, inserted] = cache_->interfaces.try_emplace(name, nullptr);
    if (inserted) {
      entry->second = isolate_->CreateInterfaceType();
    }
    interfaces_.push_back(entry->second);
  }
  for (std::size_t index = 0; index < description_->classes.size(); ++index) {
    classes_.push_back(Class(index));
  }
  // 先创建全部名义布局，再建立结构签名；允许类的签名引用自身或后声明的类。
  for (std::size_t index = 0; index < description_->classes.size(); ++index) {
    const auto& description = description_->classes.at(index);
    auto* stored = classes_.at(index);
    if (description.expected_type) {
      stored->BindExpectedType(*Signature(*description.expected_type));
    }
    if (description.callable_type) {
      stored->BindCallableType(*Signature(*description.callable_type));
    }
  }
  ProgramLayouts result;
  for (const auto index : description_->array_types) {
    result.arrays.push_back(Array(index));
  }
  for (const auto index : description_->collection_types) {
    result.collections.push_back(Collection(index));
  }
  for (const auto& type : description_->types) {
    result.types.push_back(Constraint(type));
  }
  result.classes = std::move(classes_);
  result.interfaces = std::move(interfaces_);
  return result;
}

}  // namespace

ProgramLayouts InstantiateLayouts(const layout::Program& description,
                                  Isolate& isolate,
                                  LayoutCache& cache,
                                  std::span<const ClassLayoutOptions> classes) {
  return LayoutInstantiation(description, isolate, cache, classes).Build();
}

}  // namespace vela::runtime
