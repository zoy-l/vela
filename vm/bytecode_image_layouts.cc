#include "vm/bytecode_image_internal.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "base/binary.h"
#include "base/callable.h"
#include "runtime/date.h"
#include "runtime/error.h"
#include "runtime/isolate.h"
#include "runtime/object.h"
#include "runtime/value.h"

namespace vela::vm::internal {
namespace {

std::expected<bool, BinaryError> ReadFlag(BinaryReader& reader) {
  const auto flag = reader.Read<std::uint8_t>();
  if (!flag) {
    return std::unexpected(flag.error());
  }
  if (*flag > 1) {
    return std::unexpected(reader.Error("invalid binary flag"));
  }
  return *flag != 0;
}

struct ImageClass final {
  runtime::ClassLayout* layout = nullptr;
  std::uint64_t signature = kNoImageId;
  std::uint64_t expected = kNoImageId;
};

std::expected<runtime::ClassLayoutOptions, BinaryError> ReadClassOptions(
    BinaryReader& reader,
    std::span<const runtime::InterfaceType* const> interface_types) {
  runtime::ClassLayoutOptions options;
  const auto has_callable = ReadFlag(reader);
  if (!has_callable) {
    return std::unexpected(has_callable.error());
  }
  if (*has_callable) {
    const auto callable = reader.Record<std::uint32_t, std::uint8_t>();
    if (!callable) {
      return std::unexpected(callable.error());
    }
    const auto [function, mode] = *callable;
    if (mode > static_cast<std::uint8_t>(CallableMode::InterfaceMethod)) {
      return std::unexpected(reader.Error("invalid callable mode"));
    }
    options.callable =
        CallableTarget{.function = function, .mode = static_cast<CallableMode>(mode)};
  }
  auto interfaces = ReadNumbers<std::uint64_t>(reader);
  auto dispatch = ReadNumbers<std::uint32_t>(reader);
  if (!interfaces) {
    return std::unexpected(interfaces.error());
  }
  if (!dispatch) {
    return std::unexpected(dispatch.error());
  }
  for (const auto index : *interfaces) {
    if (index >= interface_types.size()) {
      return std::unexpected(reader.Error("invalid implemented interface identity"));
    }
    options.interfaces.push_back(interface_types.subspan(static_cast<std::size_t>(index)).front());
  }
  options.dispatch_targets = std::move(*dispatch);
  const auto is_enum = ReadFlag(reader);
  if (!is_enum) {
    return std::unexpected(is_enum.error());
  }
  options.is_enum = *is_enum;
  return options;
}

bool HasValidClassReferences(std::span<const std::uint32_t> references, std::uint32_t fields) {
  return (references.empty() || std::ranges::max(references) < fields) &&
         std::ranges::is_sorted(references) &&
         std::ranges::adjacent_find(references) == references.end();
}

std::expected<ImageClass, BinaryError> ReadClass(
    BinaryReader& reader,
    runtime::Isolate& isolate,
    std::span<const runtime::InterfaceType* const> interface_types) {
  const auto standard = reader.Read<std::uint8_t>();
  const auto fields = reader.Read<std::uint32_t>();
  if (!standard) {
    return std::unexpected(standard.error());
  }
  if (*standard > static_cast<std::uint8_t>(StandardClass::DateParts)) {
    return std::unexpected(reader.Error("invalid standard class kind"));
  }
  if (!fields) {
    return std::unexpected(fields.error());
  }
  auto references = ReadNumbers<std::uint32_t>(reader);
  if (!references) {
    return std::unexpected(references.error());
  }
  if (!HasValidClassReferences(*references, *fields)) {
    return std::unexpected(reader.Error("invalid class reference-field table"));
  }
  auto parsed_options = ReadClassOptions(reader, interface_types);
  if (!parsed_options) {
    return std::unexpected(parsed_options.error());
  }
  auto options = std::move(*parsed_options);
  const auto callable_type = reader.Read<std::uint64_t>();
  const auto expected_type = reader.Read<std::uint64_t>();
  if (!callable_type) {
    return std::unexpected(callable_type.error());
  }
  if (!expected_type) {
    return std::unexpected(expected_type.error());
  }
  if (*expected_type != kNoImageId &&
      (*fields != 1 || options.callable || options.is_enum || !options.interfaces.empty() ||
       !options.dispatch_targets.empty())) {
    return std::unexpected(reader.Error("invalid Expected type layout"));
  }
  if (!options.callable && *callable_type != kNoImageId) {
    return std::unexpected(reader.Error("non-callable layout has callable type"));
  }
  if (options.is_enum && (*fields != 1 || !references->empty() || options.callable ||
                          !options.interfaces.empty() || !options.dispatch_targets.empty())) {
    return std::unexpected(reader.Error("invalid enum identity layout"));
  }
  if (*standard == static_cast<std::uint8_t>(StandardClass::Error)) {
    auto& layout = isolate.error_layout();
    if (*fields != layout.field_count() ||
        !std::ranges::equal(*references, layout.reference_fields()) || options.callable ||
        !options.interfaces.empty() || !options.dispatch_targets.empty() || options.is_enum ||
        *expected_type != kNoImageId) {
      return std::unexpected(reader.Error("invalid standard Error layout"));
    }
    return ImageClass{.layout = &layout};
  }
  if (*standard == static_cast<std::uint8_t>(StandardClass::DateParts)) {
    auto& layout = isolate.date_parts_layout();
    if (*fields != layout.field_count() ||
        !std::ranges::equal(*references, layout.reference_fields()) || options.callable ||
        !options.interfaces.empty() || !options.dispatch_targets.empty() || options.is_enum ||
        *expected_type != kNoImageId) {
      return std::unexpected(reader.Error("invalid standard DateParts layout"));
    }
    return ImageClass{.layout = &layout};
  }
  return ImageClass{
      .layout = isolate.CreateClassLayout(*fields, *references, std::move(options)),
      .signature = *callable_type,
      .expected = *expected_type,
  };
}

template <class T>
std::expected<T*, std::string> ResolvePointer(std::uint64_t index, std::span<T* const> values) {
  if (index == kNoImageId) {
    return nullptr;
  }
  if (index >= values.size() ||
      values.subspan(static_cast<std::size_t>(index)).front() == nullptr) {
    return std::unexpected("invalid layout reference");
  }
  return values.subspan(static_cast<std::size_t>(index)).front();
}

class LayoutLoader final {
 public:
  LayoutLoader(std::span<const ImageElement> arrays,
               std::span<const ImageCollection> collections,
               std::span<const ImageSignature> signatures,
               runtime::Isolate& isolate,
               LoadedLayouts& output)
      : arrays_(arrays),
        collections_(collections),
        signatures_(signatures),
        isolate_(&isolate),
        output_(&output) {}
  std::expected<void, std::string> Build();

 private:
  std::expected<void, std::string> BuildArray(std::size_t index);
  std::expected<void, std::string> BuildCollection(std::size_t index);
  std::expected<void, std::string> BuildSignature(std::size_t index);
  std::expected<void, std::string> BuildNode(std::size_t index);
  std::expected<void, std::string> AddDependencies(std::size_t owner, const ImageElement& element);
  std::expected<void, std::string> CollectDependencies();
  std::span<const ImageElement> arrays_;
  std::span<const ImageCollection> collections_;
  std::span<const ImageSignature> signatures_;
  runtime::Isolate* isolate_;
  LoadedLayouts* output_;
  std::vector<std::vector<std::size_t>> dependents_;
  std::vector<std::size_t> pending_;
};

std::expected<void, std::string> LayoutLoader::AddDependencies(std::size_t owner,
                                                               const ImageElement& element) {
  if (element.kind == static_cast<std::uint8_t>(runtime::ValueKind::Null) &&
      (element.flags & kDynamicImageFlag) == 0) {
    return std::unexpected("array/collection element type cannot be null-only");
  }
  if (element.array != kNoImageId) {
    if (element.array >= arrays_.size()) {
      return std::unexpected("invalid array-layout reference");
    }
    dependents_.at(static_cast<std::size_t>(element.array)).push_back(owner);
    ++pending_.at(owner);
  }
  if (element.collection != kNoImageId) {
    if (element.collection >= collections_.size()) {
      return std::unexpected("invalid collection-layout reference");
    }
    dependents_.at(arrays_.size() + static_cast<std::size_t>(element.collection)).push_back(owner);
    ++pending_.at(owner);
  }
  if (element.signature != kNoImageId) {
    if (element.signature >= signatures_.size()) {
      return std::unexpected("invalid type-signature reference");
    }
    dependents_
        .at(arrays_.size() + collections_.size() + static_cast<std::size_t>(element.signature))
        .push_back(owner);
    ++pending_.at(owner);
  }
  return {};
}

std::expected<void, std::string> LayoutLoader::BuildArray(std::size_t index) {
  const auto& input = arrays_.subspan(index).front();
  auto element = ResolveElement(input, *output_);
  if (!element) {
    return std::unexpected(element.error());
  }
  output_->arrays.at(index) = isolate_->CreateArrayLayout(*element);
  return {};
}

std::expected<void, std::string> LayoutLoader::BuildCollection(std::size_t index) {
  const auto& input = collections_.subspan(index).front();
  auto keys = ResolveElement(input.keys, *output_);
  if (!keys) {
    return std::unexpected(keys.error());
  }
  runtime::CollectionLayout layout{.keys = *keys};
  if (input.values) {
    auto values = ResolveElement(*input.values, *output_);
    if (!values) {
      return std::unexpected(values.error());
    }
    layout.values = *values;
  }
  output_->collections.at(index) = isolate_->CreateCollectionLayout(layout);
  return {};
}

std::expected<runtime::TypeConstraint, std::string> ResolveSignatureType(
    const ImageElement& input,
    const LoadedLayouts& layouts) {
  const auto type = ResolveType(input, layouts);
  if (!type) {
    return type;
  }
  const auto kind = type->object_kind;
  if (type->kind == runtime::ValueKind::Object && type->signature == nullptr &&
      (kind == runtime::ObjectKind::Future || kind == runtime::ObjectKind::Completer ||
       kind == runtime::ObjectKind::Expected)) {
    return std::unexpected("generic signature type must retain its payload signature");
  }
  return type;
}

std::expected<void, std::string> LayoutLoader::BuildSignature(std::size_t index) {
  const auto& input = signatures_.subspan(index).front();
  runtime::TypeSignature signature;
  for (const auto& parameter : input.parameters) {
    const auto type = ResolveSignatureType(parameter.type, *output_);
    if (!type) {
      return std::unexpected(type.error());
    }
    signature.parameters.push_back(
        {.type = *type, .optional = parameter.optional, .rest = parameter.rest});
  }
  if (input.result) {
    const auto type = ResolveSignatureType(*input.result, *output_);
    if (!type) {
      return std::unexpected(type.error());
    }
    signature.result = *type;
  }
  output_->signatures.at(index) = isolate_->CreateTypeSignature(std::move(signature));
  return {};
}

std::expected<void, std::string> LayoutLoader::CollectDependencies() {
  for (std::size_t index = 0; index < arrays_.size(); ++index) {
    if (auto built = AddDependencies(index, arrays_.subspan(index).front()); !built) {
      return built;
    }
  }
  for (std::size_t index = 0; index < collections_.size(); ++index) {
    const auto& input = collections_.subspan(index).front();
    const auto owner = arrays_.size() + index;
    if (auto built = AddDependencies(owner, input.keys); !built) {
      return built;
    }
    if (input.values) {
      if (auto built = AddDependencies(owner, *input.values); !built) {
        return built;
      }
    }
  }
  for (std::size_t index = 0; index < signatures_.size(); ++index) {
    const auto& input = signatures_.subspan(index).front();
    const auto owner = arrays_.size() + collections_.size() + index;
    for (const auto& parameter : input.parameters) {
      if (auto built = AddDependencies(owner, parameter.type); !built) {
        return built;
      }
    }
    if (const auto result = input.result) {
      if (auto built = AddDependencies(owner, *result); !built) {
        return built;
      }
    }
  }
  return {};
}

std::expected<void, std::string> LayoutLoader::Build() {
  const auto count = arrays_.size() + collections_.size() + signatures_.size();
  dependents_.resize(count);
  pending_.resize(count);
  if (auto collected = CollectDependencies(); !collected) {
    return collected;
  }
  std::vector<std::size_t> ready;
  ready.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    if (pending_.at(index) == 0) {
      ready.push_back(index);
    }
  }
  // 元数据来自外部镜像；拓扑构造同时拒绝循环，且不让嵌套深度消耗 C++ 调用栈。
  for (std::size_t cursor = 0; cursor < ready.size(); ++cursor) {
    const auto index = ready.at(cursor);
    const auto built = BuildNode(index);
    if (!built) {
      return built;
    }
    for (const auto dependent : dependents_.at(index)) {
      if (--pending_.at(dependent) == 0) {
        ready.push_back(dependent);
      }
    }
  }
  if (ready.size() != count) {
    return std::unexpected("cyclic runtime type layouts");
  }
  return {};
}

std::expected<void, std::string> LayoutLoader::BuildNode(std::size_t index) {
  if (index < arrays_.size()) {
    return BuildArray(index);
  }
  index -= arrays_.size();
  if (index < collections_.size()) {
    return BuildCollection(index);
  }
  return BuildSignature(index - collections_.size());
}

bool HasExpectedReference(const runtime::TypeConstraint& type) {
  return type.dynamic || type.kind == runtime::ValueKind::String ||
         (type.kind == runtime::ValueKind::Object &&
          (type.nominal == nullptr || !type.nominal->IsEnum()));
}

bool IsValidExpectedLayout(const runtime::ClassLayout& layout,
                           const runtime::TypeSignature& signature) {
  if (signature.parameters.size() != 1) {
    return false;
  }
  const auto& error = signature.parameters.front();
  if (error.optional || error.rest) {
    return false;
  }
  const auto value = signature.result;
  const bool reference =
      HasExpectedReference(error.type) || (value && HasExpectedReference(*value));
  return layout.reference_fields().size() == (reference ? 1U : 0U);
}

std::expected<ImageCollection, BinaryError> ReadCollection(BinaryReader& reader) {
  const auto keys = ReadElement(reader);
  const auto has_values = ReadFlag(reader);
  if (!keys) {
    return std::unexpected(keys.error());
  }
  if (!has_values) {
    return std::unexpected(has_values.error());
  }
  ImageCollection input{.keys = *keys, .values = std::nullopt};
  if (*has_values) {
    const auto value = ReadElement(reader);
    if (!value) {
      return std::unexpected(value.error());
    }
    input.values = *value;
  }
  return input;
}

std::expected<ImageSignature::Parameter, BinaryError> ReadSignatureParameter(BinaryReader& reader) {
  const auto type = ReadElement(reader);
  const auto optional = ReadFlag(reader);
  const auto rest = ReadFlag(reader);
  if (!type) {
    return std::unexpected(type.error());
  }
  if (!optional) {
    return std::unexpected(optional.error());
  }
  if (!rest) {
    return std::unexpected(rest.error());
  }
  return ImageSignature::Parameter{.type = *type, .optional = *optional, .rest = *rest};
}

std::expected<ImageSignature, BinaryError> ReadSignature(BinaryReader& reader) {
  auto parameters =
      ReadRecords<ImageSignature::Parameter>(reader, kElementBytes + 2, ReadSignatureParameter);
  if (!parameters) {
    return std::unexpected(parameters.error());
  }
  ImageSignature signature{.parameters = std::move(*parameters)};
  const auto has_result = ReadFlag(reader);
  if (!has_result) {
    return std::unexpected(has_result.error());
  }
  if (*has_result) {
    const auto result = ReadElement(reader);
    if (!result) {
      return std::unexpected(result.error());
    }
    signature.result = *result;
  }
  return signature;
}

std::expected<void, BinaryError> BindClassTypes(BinaryReader& reader,
                                                std::span<const ImageClass> classes,
                                                LoadedLayouts& layouts) {
  for (const auto& input : classes) {
    const auto signature = ResolvePointer(
        input.signature, std::span<const runtime::TypeSignature* const>(layouts.signatures));
    if (!signature) {
      return std::unexpected(reader.Error(signature.error()));
    }
    if (*signature != nullptr) {
      input.layout->BindCallableType(**signature);
    }
    const auto expected = ResolvePointer(
        input.expected, std::span<const runtime::TypeSignature* const>(layouts.signatures));
    if (!expected) {
      return std::unexpected(reader.Error(expected.error()));
    }
    if (*expected != nullptr) {
      if (!IsValidExpectedLayout(*input.layout, **expected)) {
        return std::unexpected(
            reader.Error("Expected signature does not match its payload storage"));
      }
      input.layout->BindExpectedType(**expected);
    }
  }
  return {};
}

}  // namespace

std::expected<std::vector<std::string>, BinaryError> ReadTexts(BinaryReader& reader) {
  const auto count = reader.Count(sizeof(std::uint64_t));
  if (!count) {
    return std::unexpected(count.error());
  }
  std::vector<std::string> texts;
  texts.reserve(*count);
  for (std::size_t index = 0; index < *count; ++index) {
    auto text = reader.Text();
    if (!text) {
      return std::unexpected(text.error());
    }
    texts.push_back(std::move(*text));
  }
  return texts;
}

std::expected<ImageElement, BinaryError> ReadElement(BinaryReader& reader) {
  const auto record = reader.Record<std::uint8_t, std::uint8_t, std::uint8_t, std::uint64_t,
                                    std::uint64_t, std::uint64_t, std::uint64_t>();
  if (!record) {
    return std::unexpected(record.error());
  }
  const auto [kind, object_kind, flags, interface, instance, array, collection] = *record;
  const auto signature = reader.Read<std::uint64_t>();
  if (!signature) {
    return std::unexpected(signature.error());
  }
  constexpr auto allowed_flags =
      kNullableImageFlag | kCallableImageFlag | kInterfaceImageFlag | kDynamicImageFlag;
  if (kind > static_cast<std::uint8_t>(runtime::ValueKind::String) ||
      object_kind > static_cast<std::uint8_t>(runtime::ObjectKind::RegExpMatch) ||
      flags > allowed_flags) {
    return std::unexpected(reader.Error("invalid element kind or flags"));
  }
  if (((flags & kInterfaceImageFlag) != 0) != (interface != kNoImageId)) {
    return std::unexpected(reader.Error("invalid interface type reference"));
  }
  if ((flags & kDynamicImageFlag) != 0 &&
      (kind != static_cast<std::uint8_t>(runtime::ValueKind::Null) ||
       object_kind != static_cast<std::uint8_t>(runtime::ObjectKind::Instance) ||
       flags != kDynamicImageFlag || interface != kNoImageId || instance != kNoImageId ||
       array != kNoImageId || collection != kNoImageId || *signature != kNoImageId)) {
    return std::unexpected(reader.Error("dynamic type cannot carry concrete type metadata"));
  }
  if (*signature != kNoImageId && (flags & kCallableImageFlag) == 0 &&
      object_kind != static_cast<std::uint8_t>(runtime::ObjectKind::Future) &&
      object_kind != static_cast<std::uint8_t>(runtime::ObjectKind::Expected) &&
      object_kind != static_cast<std::uint8_t>(runtime::ObjectKind::Completer)) {
    return std::unexpected(
        reader.Error("type signature requires a callable, Expected, Future or Completer"));
  }
  return ImageElement{
      .kind = kind,
      .object_kind = object_kind,
      .flags = flags,
      .interface = interface,
      .instance = instance,
      .array = array,
      .collection = collection,
      .signature = *signature,
  };
}

std::expected<runtime::ArrayLayout, std::string> ResolveElement(const ImageElement& input,
                                                                const LoadedLayouts& layouts) {
  const auto instance = ResolvePointer(input.instance, std::span(layouts.classes));
  const auto array = ResolvePointer(input.array, std::span(layouts.arrays));
  const auto collection = ResolvePointer(input.collection, std::span(layouts.collections));
  const auto interface = ResolvePointer(input.interface, std::span(layouts.interfaces));
  const auto signature = ResolvePointer(input.signature, std::span(layouts.signatures));
  if (!instance) {
    return std::unexpected(instance.error());
  }
  if (!array) {
    return std::unexpected(array.error());
  }
  if (!collection) {
    return std::unexpected(collection.error());
  }
  if (!signature) {
    return std::unexpected(signature.error());
  }
  if (!interface) {
    return std::unexpected(interface.error());
  }
  if (input.kind == static_cast<std::uint8_t>(runtime::ValueKind::Integer) &&
      *instance != nullptr && !(*instance)->IsEnum()) {
    return std::unexpected("integer storage identity must be an enum");
  }
  if (*signature != nullptr && (input.flags & kCallableImageFlag) == 0) {
    const auto parameters = std::span((*signature)->parameters);
    const auto expected =
        input.object_kind == static_cast<std::uint8_t>(runtime::ObjectKind::Expected);
    if (parameters.size() != (expected ? 1U : 0U) ||
        (expected && (parameters.front().optional || parameters.front().rest))) {
      return std::unexpected("invalid generic payload signature");
    }
  }
  return runtime::ArrayLayout{
      .element =
          {
              .kind = static_cast<runtime::ValueKind>(input.kind),
              .object_kind = static_cast<runtime::ObjectKind>(input.object_kind),
              .nominal = *instance == nullptr ? nullptr : &(*instance)->nominal_type(),
              .array = *array,
              .collection = *collection,
              .interface = *interface,
              .nullable = (input.flags & kNullableImageFlag) != 0,
              .callable = (input.flags & kCallableImageFlag) != 0,
              .dynamic = (input.flags & kDynamicImageFlag) != 0,
              .signature = *signature,
          },
  };
}

std::expected<runtime::TypeConstraint, std::string> ResolveType(const ImageElement& input,
                                                                const LoadedLayouts& layouts) {
  const auto element = ResolveElement(input, layouts);
  if (!element) {
    return std::unexpected(element.error());
  }
  return element->element_type();
}

std::expected<LoadedLayouts, BinaryError> ReadLayouts(BinaryReader& reader,
                                                      runtime::Isolate& isolate) {
  LoadedLayouts layouts;
  const auto interfaces = reader.Read<std::uint64_t>();
  if (!interfaces) {
    return std::unexpected(interfaces.error());
  }
  // 身份本身不携带数据；至少一个序列化引用为其资源规模提供边界。
  if (*interfaces > (reader.offset() + reader.remaining()) / sizeof(std::uint64_t)) {
    return std::unexpected(reader.Error("interface identity count exceeds input size"));
  }
  for (std::uint64_t index = 0; index < *interfaces; ++index) {
    layouts.interfaces.push_back(isolate.CreateInterfaceType());
  }
  constexpr auto kClassBytes = (5 * sizeof(std::uint64_t)) + sizeof(std::uint32_t) + 3;
  const auto classes =
      ReadRecords<ImageClass>(reader, kClassBytes, [&isolate, &layouts](BinaryReader& input) {
        return ReadClass(input, isolate, layouts.interfaces);
      });
  if (!classes) {
    return std::unexpected(classes.error());
  }
  for (const auto& input : *classes) {
    layouts.classes.push_back(input.layout);
  }
  const auto arrays = ReadRecords<ImageElement>(reader, kElementBytes, ReadElement);
  if (!arrays) {
    return std::unexpected(arrays.error());
  }
  const auto collections = ReadRecords<ImageCollection>(reader, kElementBytes + 1, ReadCollection);
  if (!collections) {
    return std::unexpected(collections.error());
  }
  const auto signatures =
      ReadRecords<ImageSignature>(reader, sizeof(std::uint64_t) + 1, ReadSignature);
  if (!signatures) {
    return std::unexpected(signatures.error());
  }
  layouts.arrays.resize(arrays->size());
  layouts.collections.resize(collections->size());
  layouts.signatures.resize(signatures->size());
  if (auto built = LayoutLoader(*arrays, *collections, *signatures, isolate, layouts).Build();
      !built) {
    return std::unexpected(reader.Error(built.error()));
  }
  if (auto bound = BindClassTypes(reader, *classes, layouts); !bound) {
    return std::unexpected(bound.error());
  }
  return layouts;
}

}  // namespace vela::vm::internal
