#include "runtime/isolate.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include "base/utf8.h"
#include "runtime/date.h"
#include "runtime/error.h"
#include "runtime/native_function.h"
#include "runtime/object.h"
#include "runtime/standard_library.h"

namespace vela::runtime {
namespace {

template <class T>
const T* InternLayout(T layout,
                      absl::flat_hash_map<T, const T*>& index,
                      std::vector<std::unique_ptr<T>>& storage) {
  const auto [entry, inserted] = index.try_emplace(layout, nullptr);
  if (inserted) {
    auto owned = std::make_unique<T>(std::move(layout));
    entry->second = owned.get();
    storage.push_back(std::move(owned));
  }
  return entry->second;
}

}  // namespace

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
  date_parts_layout_ = CreateClassLayout(kDatePartsFieldCount, {});
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
  std::ranges::sort(options.interfaces, std::less{});
  result->interfaces_ = std::move(options.interfaces);
  result->dispatch_targets_ = std::move(options.dispatch_targets);
  result->is_enum_ = options.is_enum;
  result->callable_type_ = options.callable_type;
  result->expected_type_ = options.expected_type;
  class_layouts_.push_back(std::move(layout));
  return result;
}

const ArrayLayout* Isolate::CreateArrayLayout(ArrayLayout layout) {
  if (layout.element.array != nullptr) {
    layout.element.object_kind = ObjectKind::Array;
  }
  return InternLayout(layout, array_layout_index_, array_layouts_);
}

const CollectionLayout* Isolate::CreateCollectionLayout(CollectionLayout layout) {
  return InternLayout(layout, collection_layout_index_, collection_layouts_);
}

const TypeSignature* Isolate::CreateTypeSignature(TypeSignature signature) {
  return InternLayout(std::move(signature), type_signature_index_, type_signatures_);
}

const InterfaceType* Isolate::CreateInterfaceType() {
  auto type = std::make_unique<InterfaceType>();
  const auto* result = type.get();
  interface_types_.push_back(std::move(type));
  return result;
}

const CodeIdentity* Isolate::CreateCodeIdentity() {
  auto identity = std::make_unique<CodeIdentity>();
  const auto* result = identity.get();
  code_identities_.push_back(std::move(identity));
  return result;
}

LayoutCounts Isolate::layout_counts() const noexcept {
  return {
      .classes = class_layouts_.size(),
      .arrays = array_layouts_.size(),
      .collections = collection_layouts_.size(),
      .signatures = type_signatures_.size(),
      .interfaces = interface_types_.size(),
      .code_identities = code_identities_.size(),
  };
}

}  // namespace vela::runtime
