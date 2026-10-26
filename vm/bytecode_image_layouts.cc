#include "vm/bytecode_image_internal.h"

#include <algorithm>
#include <array>
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

std::expected<runtime::ClassLayout*, BinaryError> ReadClass(BinaryReader& reader,
                                                            runtime::Isolate& isolate) {
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
  if ((!references->empty() && *std::ranges::max_element(*references) >= *fields) ||
      !std::ranges::is_sorted(*references) ||
      std::ranges::adjacent_find(*references) != references->end()) {
    return std::unexpected(reader.Error("invalid class reference-field table"));
  }
  const auto has_callable = ReadFlag(reader);
  if (!has_callable) {
    return std::unexpected(has_callable.error());
  }
  runtime::ClassLayoutOptions options;
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
  auto interfaces = ReadNumbers<std::uint32_t>(reader);
  auto dispatch = ReadNumbers<std::uint32_t>(reader);
  if (!interfaces) {
    return std::unexpected(interfaces.error());
  }
  if (!dispatch) {
    return std::unexpected(dispatch.error());
  }
  options.interfaces = std::move(*interfaces);
  options.dispatch_targets = std::move(*dispatch);
  if (*standard == static_cast<std::uint8_t>(StandardClass::Error)) {
    constexpr auto error_fields =
        std::to_array({runtime::kErrorMessageField, runtime::kErrorCodeField});
    if (*fields != runtime::kErrorFieldCount || !std::ranges::equal(*references, error_fields) ||
        options.callable || !options.interfaces.empty() || !options.dispatch_targets.empty()) {
      return std::unexpected(reader.Error("invalid standard Error layout"));
    }
    return &isolate.error_layout();
  }
  if (*standard == static_cast<std::uint8_t>(StandardClass::DateParts)) {
    if (*fields != runtime::kDatePartsFieldCount || !references->empty() || options.callable ||
        !options.interfaces.empty() || !options.dispatch_targets.empty()) {
      return std::unexpected(reader.Error("invalid standard DateParts layout"));
    }
    return &isolate.date_parts_layout();
  }
  return isolate.CreateClassLayout(*fields, *references, std::move(options));
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
               runtime::Isolate& isolate,
               LoadedLayouts& output)
      : arrays_(arrays), collections_(collections), isolate_(&isolate), output_(&output) {}
  std::expected<void, std::string> Build();

 private:
  std::expected<void, std::string> BuildArray(std::size_t index);
  std::expected<void, std::string> BuildCollection(std::size_t index);
  std::expected<void, std::string> BuildElement(const ImageElement& element);
  std::span<const ImageElement> arrays_;
  std::span<const ImageCollection> collections_;
  runtime::Isolate* isolate_;
  LoadedLayouts* output_;
  std::vector<std::uint8_t> array_states_;
  std::vector<std::uint8_t> collection_states_;
};

std::expected<void, std::string> LayoutLoader::BuildElement(const ImageElement& element) {
  if (element.kind == static_cast<std::uint8_t>(runtime::ValueKind::Null)) {
    return std::unexpected("array/collection element type cannot be null-only");
  }
  if (element.array != kNoImageId) {
    if (element.array >= arrays_.size()) {
      return std::unexpected("invalid array-layout reference");
    }
    if (auto built = BuildArray(static_cast<std::size_t>(element.array)); !built) {
      return built;
    }
  }
  if (element.collection != kNoImageId) {
    if (element.collection >= collections_.size()) {
      return std::unexpected("invalid collection-layout reference");
    }
    if (auto built = BuildCollection(static_cast<std::size_t>(element.collection)); !built) {
      return built;
    }
  }
  return {};
}

std::expected<void, std::string> LayoutLoader::BuildArray(std::size_t index) {
  if (array_states_.at(index) == 2) {
    return {};
  }
  if (array_states_.at(index) == 1) {
    return std::unexpected("cyclic array/collection layouts");
  }
  array_states_.at(index) = 1;
  const auto& input = arrays_.subspan(index).front();
  if (auto built = BuildElement(input); !built) {
    return built;
  }
  auto element = ResolveElement(input, *output_);
  if (!element) {
    return std::unexpected(element.error());
  }
  output_->arrays.at(index) = isolate_->CreateArrayLayout(*element);
  array_states_.at(index) = 2;
  return {};
}

std::expected<void, std::string> LayoutLoader::BuildCollection(std::size_t index) {
  if (collection_states_.at(index) == 2) {
    return {};
  }
  if (collection_states_.at(index) == 1) {
    return std::unexpected("cyclic array/collection layouts");
  }
  collection_states_.at(index) = 1;
  const auto& input = collections_.subspan(index).front();
  if (auto built = BuildElement(input.keys); !built) {
    return built;
  }
  if (input.values) {
    if (auto built = BuildElement(*input.values); !built) {
      return built;
    }
  }
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
  collection_states_.at(index) = 2;
  return {};
}

std::expected<void, std::string> LayoutLoader::Build() {
  array_states_.assign(arrays_.size(), 0);
  collection_states_.assign(collections_.size(), 0);
  for (std::size_t index = 0; index < arrays_.size(); ++index) {
    if (auto built = BuildArray(index); !built) {
      return built;
    }
  }
  for (std::size_t index = 0; index < collections_.size(); ++index) {
    if (auto built = BuildCollection(index); !built) {
      return built;
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
  const auto record = reader.Record<std::uint8_t, std::uint8_t, std::uint8_t, std::uint32_t,
                                    std::uint64_t, std::uint64_t, std::uint64_t>();
  if (!record) {
    return std::unexpected(record.error());
  }
  const auto [kind, object_kind, flags, interface, instance, array, collection] = *record;
  constexpr auto allowed_flags = kNullableImageFlag | kCallableImageFlag | kInterfaceImageFlag;
  if (kind > static_cast<std::uint8_t>(runtime::ValueKind::String) ||
      object_kind > static_cast<std::uint8_t>(runtime::ObjectKind::ByteData) ||
      flags > allowed_flags) {
    return std::unexpected(reader.Error("invalid element kind or flags"));
  }
  return ImageElement{
      .kind = kind,
      .object_kind = object_kind,
      .flags = flags,
      .interface = interface,
      .instance = instance,
      .array = array,
      .collection = collection,
  };
}

std::expected<runtime::ArrayLayout, std::string> ResolveElement(const ImageElement& input,
                                                                const LoadedLayouts& layouts) {
  const auto instance = ResolvePointer(input.instance, std::span(layouts.classes));
  const auto array = ResolvePointer(input.array, std::span(layouts.arrays));
  const auto collection = ResolvePointer(input.collection, std::span(layouts.collections));
  if (!instance) {
    return std::unexpected(instance.error());
  }
  if (!array) {
    return std::unexpected(array.error());
  }
  if (!collection) {
    return std::unexpected(collection.error());
  }
  return runtime::ArrayLayout{
      .element_kind = static_cast<runtime::ValueKind>(input.kind),
      .element_class = *instance,
      .element_array = *array,
      .element_object_kind = static_cast<runtime::ObjectKind>(input.object_kind),
      .element_interface =
          (input.flags & kInterfaceImageFlag) != 0 ? std::optional(input.interface) : std::nullopt,
      .element_collection = *collection,
      .callable_elements = (input.flags & kCallableImageFlag) != 0,
      .nullable_elements = (input.flags & kNullableImageFlag) != 0,
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
  const auto classes = reader.Count((3 * sizeof(std::uint64_t)) + sizeof(std::uint32_t) + 2);
  if (!classes) {
    return std::unexpected(classes.error());
  }
  layouts.classes.reserve(*classes);
  for (std::size_t index = 0; index < *classes; ++index) {
    const auto layout = ReadClass(reader, isolate);
    if (!layout) {
      return std::unexpected(layout.error());
    }
    layouts.classes.push_back(*layout);
  }
  const auto arrays = reader.Count(kElementBytes);
  if (!arrays) {
    return std::unexpected(arrays.error());
  }
  std::vector<ImageElement> array_inputs;
  for (std::size_t index = 0; index < *arrays; ++index) {
    const auto element = ReadElement(reader);
    if (!element) {
      return std::unexpected(element.error());
    }
    array_inputs.push_back(*element);
  }
  const auto collections = reader.Count(kElementBytes + 1);
  if (!collections) {
    return std::unexpected(collections.error());
  }
  std::vector<ImageCollection> collection_inputs;
  for (std::size_t index = 0; index < *collections; ++index) {
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
      const auto values = ReadElement(reader);
      if (!values) {
        return std::unexpected(values.error());
      }
      input.values = *values;
    }
    collection_inputs.push_back(input);
  }
  layouts.arrays.resize(*arrays);
  layouts.collections.resize(*collections);
  if (auto built = LayoutLoader(array_inputs, collection_inputs, isolate, layouts).Build();
      !built) {
    return std::unexpected(reader.Error(built.error()));
  }
  return layouts;
}

}  // namespace vela::vm::internal
