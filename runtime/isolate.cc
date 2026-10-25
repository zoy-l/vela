#include "runtime/isolate.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "base/utf8.h"
#include "runtime/error.h"
#include "runtime/native_function.h"
#include "runtime/object.h"
#include "runtime/standard_library.h"

namespace vela::runtime {

auto Isolate::RegisterNative(NativeFunction function)
    -> std::expected<void, NativeRegistrationError> {
  const auto name = function.name();
  if (name.empty() || name.contains('\0') || InvalidUtf8Offset(name)) {
    return std::unexpected(NativeRegistrationError::InvalidName);
  }
  if (natives_.contains(name)) {
    return std::unexpected(NativeRegistrationError::DuplicateName);
  }
  std::string key(name);
  natives_.emplace(std::move(key), std::make_shared<const NativeFunction>(std::move(function)));
  return {};
}

auto Isolate::FindNative(std::string_view name) const -> std::shared_ptr<const NativeFunction> {
  const auto found = natives_.find(name);
  return found == natives_.end() ? FindStandardNative(name) : found->second;
}

Isolate::Isolate(std::size_t gc_threshold) noexcept : heap_(gc_threshold) {
  constexpr auto references = std::to_array({kErrorMessageField, kErrorCodeField});
  error_layout_ = CreateClassLayout(kErrorFieldCount, references);
}

auto Isolate::input() noexcept -> std::istream& {
  return input_ != nullptr ? *input_ : std::cin;
}

auto Isolate::output() noexcept -> std::ostream& {
  return output_ != nullptr ? *output_ : std::cout;
}

void Isolate::SetConsole(std::istream& input, std::ostream& output) noexcept {
  input_ = &input;
  output_ = &output;
}

auto Isolate::CreateClassLayout(std::uint32_t field_count,
                                std::span<const std::uint32_t> reference_fields,
                                ClassLayoutOptions options) -> ClassLayout* {
  std::vector<std::uint32_t> references(reference_fields.begin(), reference_fields.end());
  std::ranges::sort(references);
  const auto duplicates = std::ranges::unique(references);
  references.erase(duplicates.begin(), duplicates.end());
  std::erase_if(references,
                [field_count](std::uint32_t index) -> bool { return index >= field_count; });

  std::unique_ptr<ClassLayout> layout(new ClassLayout(field_count, std::move(references)));
  ClassLayout* result = layout.get();
  result->callable_ = options.callable;
  std::ranges::sort(options.interfaces);
  result->interfaces_ = std::move(options.interfaces);
  result->dispatch_targets_ = std::move(options.dispatch_targets);
  class_layouts_.push_back(std::move(layout));
  return result;
}

auto Isolate::CreateArrayLayout(ArrayLayout layout) -> const ArrayLayout* {
  if (layout.element_array != nullptr) {
    layout.element_object_kind = ObjectKind::Array;
  }
  auto stored = std::make_unique<ArrayLayout>(layout);
  const auto* result = stored.get();
  array_layouts_.push_back(std::move(stored));
  return result;
}

auto Isolate::CreateCollectionLayout(CollectionLayout layout) -> const CollectionLayout* {
  auto stored = std::make_unique<CollectionLayout>(layout);
  const auto* result = stored.get();
  collection_layouts_.push_back(std::move(stored));
  return result;
}

auto Isolate::layout_counts() const noexcept -> LayoutCounts {
  return {
      .classes = class_layouts_.size(),
      .arrays = array_layouts_.size(),
      .collections = collection_layouts_.size(),
  };
}

}  // namespace vela::runtime
