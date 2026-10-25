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
  const auto standard_error = ReadFlag(reader);
  const auto fields = reader.Read<std::uint32_t>();
  if (!standard_error) {
    return std::unexpected(standard_error.error());
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
  if (*standard_error) {
    constexpr auto error_fields =
        std::to_array({runtime::kErrorMessageField, runtime::kErrorCodeField});
    if (*fields != runtime::kErrorFieldCount || !std::ranges::equal(*references, error_fields) ||
        options.callable || !options.interfaces.empty() || !options.dispatch_targets.empty()) {
      return std::unexpected(reader.Error("invalid standard Error layout"));
    }
    return &isolate.error_layout();
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

class LayoutGraph final {
 public:
  LayoutGraph(std::span<const ImageElement> arrays, std::span<const ImageCollection> collections)
      : arrays_(arrays),
        collections_(collections),
        dependencies_(arrays.size() + collections.size()),
        dependents_(dependencies_.size()) {}
  std::expected<void, std::string> Build(runtime::Isolate& isolate, LoadedLayouts& output);

 private:
  std::expected<void, std::string> Link(const ImageElement& element, std::size_t node);
  std::expected<void, std::string> Create(std::size_t node,
                                          runtime::Isolate& isolate,
                                          LoadedLayouts& output);
  std::span<const ImageElement> arrays_;
  std::span<const ImageCollection> collections_;
  std::vector<std::size_t> dependencies_;
  std::vector<std::vector<std::size_t>> dependents_;
};

std::expected<void, std::string> LayoutGraph::Link(const ImageElement& element, std::size_t node) {
  if (element.kind == static_cast<std::uint8_t>(runtime::ValueKind::Null)) {
    return std::unexpected("array/collection element type cannot be null-only");
  }
  if (element.array != kNoImageId) {
    if (element.array >= arrays_.size()) {
      return std::unexpected("invalid array-layout reference");
    }
    ++dependencies_.at(node);
    dependents_.at(static_cast<std::size_t>(element.array)).push_back(node);
  }
  if (element.collection != kNoImageId) {
    if (element.collection >= collections_.size()) {
      return std::unexpected("invalid collection-layout reference");
    }
    ++dependencies_.at(node);
    dependents_.at(arrays_.size() + static_cast<std::size_t>(element.collection)).push_back(node);
  }
  return {};
}

std::expected<void, std::string> LayoutGraph::Create(std::size_t node,
                                                     runtime::Isolate& isolate,
                                                     LoadedLayouts& output) {
  if (node < arrays_.size()) {
    auto element = ResolveElement(arrays_.subspan(node).front(), output);
    if (!element) {
      return std::unexpected(element.error());
    }
    output.arrays.at(node) = isolate.CreateArrayLayout(*element);
    return {};
  }
  const auto index = node - arrays_.size();
  const auto& collection = collections_.subspan(index).front();
  auto keys = ResolveElement(collection.keys, output);
  if (!keys) {
    return std::unexpected(keys.error());
  }
  runtime::CollectionLayout layout{.keys = *keys};
  if (collection.values) {
    auto values = ResolveElement(*collection.values, output);
    if (!values) {
      return std::unexpected(values.error());
    }
    layout.values = *values;
  }
  output.collections.at(index) = isolate.CreateCollectionLayout(layout);
  return {};
}

std::expected<void, std::string> LayoutGraph::Build(runtime::Isolate& isolate,
                                                    LoadedLayouts& output) {
  for (std::size_t index = 0; index < arrays_.size(); ++index) {
    if (auto linked = Link(arrays_.subspan(index).front(), index); !linked) {
      return linked;
    }
  }
  for (std::size_t index = 0; index < collections_.size(); ++index) {
    const auto& collection = collections_.subspan(index).front();
    const auto node = arrays_.size() + index;
    if (auto linked = Link(collection.keys, node); !linked) {
      return linked;
    }
    if (collection.values) {
      if (auto linked = Link(*collection.values, node); !linked) {
        return linked;
      }
    }
  }
  std::vector<std::size_t> ready;
  for (std::size_t index = 0; index < dependencies_.size(); ++index) {
    if (dependencies_.at(index) == 0) {
      ready.push_back(index);
    }
  }
  std::size_t completed = 0;
  while (!ready.empty()) {
    const auto node = ready.back();
    ready.pop_back();
    if (auto created = Create(node, isolate, output); !created) {
      return created;
    }
    ++completed;
    for (const auto dependent : dependents_.at(node)) {
      if (--dependencies_.at(dependent) == 0) {
        ready.push_back(dependent);
      }
    }
  }
  if (completed != dependencies_.size()) {
    return std::unexpected("cyclic array/collection layouts");
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
      object_kind > static_cast<std::uint8_t>(runtime::ObjectKind::Exception) ||
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
  if (auto built = LayoutGraph(array_inputs, collection_inputs).Build(isolate, layouts); !built) {
    return std::unexpected(reader.Error(built.error()));
  }
  return layouts;
}

}  // namespace vela::vm::internal
