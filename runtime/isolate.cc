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

std::expected<void, NativeRegistrationError> Isolate::RegisterNative(NativeFunction function) {
  const auto name = function.name();
  if (name.empty() || name.contains('\0') || InvalidUtf8Offset(name)) {
    return std::unexpected(NativeRegistrationError::InvalidName);
  }
  if (natives_.contains(name)) {
    return std::unexpected(NativeRegistrationError::DuplicateName);
  }
  std::string key(name);
  natives_.emplace(std::move(key), std::make_unique<const NativeFunction>(std::move(function)));
  return {};
}

std::shared_ptr<const NativeFunction> Isolate::FindNative(std::string_view name) const {
  const auto found = natives_.find(name);
  return found == natives_.end() ? FindStandardNative(name) : found->second;
}

Isolate::Isolate(std::size_t gc_threshold) noexcept : heap_(gc_threshold) {
  constexpr auto references = std::to_array({kErrorMessageField, kErrorCodeField});
  error_layout_ = CreateClassLayout(kErrorFieldCount, references);
}

Isolate::~Isolate() {
  // 先释放排队回调的根，再销毁堆；堆中挂起帧的析构仍可访问调度器。
  scheduler_.Shutdown();
}

std::ostream& Isolate::output() noexcept {
  return output_ != nullptr ? *output_ : std::cout;
}

void Isolate::SetOutput(std::ostream& output) noexcept {
  output_ = &output;
}

ClassLayout* Isolate::CreateClassLayout(std::uint32_t field_count,
                                        std::span<const std::uint32_t> reference_fields,
                                        ClassLayoutOptions options) {
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

const ArrayLayout* Isolate::CreateArrayLayout(ArrayLayout layout) {
  if (layout.element_array != nullptr) {
    layout.element_object_kind = ObjectKind::Array;
  }
  auto stored = std::make_unique<ArrayLayout>(layout);
  const auto* result = stored.get();
  array_layouts_.push_back(std::move(stored));
  return result;
}

const CollectionLayout* Isolate::CreateCollectionLayout(CollectionLayout layout) {
  auto stored = std::make_unique<CollectionLayout>(layout);
  const auto* result = stored.get();
  collection_layouts_.push_back(std::move(stored));
  return result;
}

LayoutCounts Isolate::layout_counts() const noexcept {
  return {
      .classes = class_layouts_.size(),
      .arrays = array_layouts_.size(),
      .collections = collection_layouts_.size(),
  };
}

}  // namespace vela::runtime
