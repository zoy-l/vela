#include "runtime/isolate/isolate.h"

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

#include <absl/container/flat_hash_map.h>
#include "base/utf8.h"
#include "runtime/exceptions/error.h"
#include "runtime/isolate/external_resources.h"
#include "runtime/library/date.h"
#include "runtime/library/standard_library.h"
#include "runtime/native/native_function.h"
#include "runtime/value/object.h"

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
  if (bindings_frozen_) {
    return std::unexpected(NativeRegistrationError::BindingsFrozen);
  }
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

NativeBindings Isolate::FreezeBindings() {
  NativeBindings result;
  result.functions_ = natives_;
  bindings_frozen_ = true;
  return result;
}

std::shared_ptr<const NativeFunction> Isolate::FindNative(std::string_view name) const {
  const auto found = natives_.find(name);
  return found == natives_.end() ? FindStandardNative(name) : found->second;
}

Isolate::Isolate(std::size_t gc_threshold) noexcept
    : heap_(*this, gc_threshold), native_dispatch_(*this) {
  constexpr auto references = std::to_array({kErrorMessageField, kErrorCodeField});
  error_layout_ = CreateClassLayout(kErrorFieldCount, references);
  date_parts_layout_ = CreateClassLayout(kDatePartsFieldCount, {});
}

Isolate::Isolate(NativeBindings bindings, std::size_t gc_threshold) noexcept
    : Isolate(gc_threshold) {
  natives_ = std::move(bindings.functions_);
  bindings_frozen_ = true;
}

Isolate::~Isolate() {
  // 先释放排队回调的根，再销毁堆；堆中挂起帧的析构仍可访问调度器。
  scheduler_.Shutdown();
  native_dispatch_.Close();
}

std::vector<ExternalResourceInfo> Isolate::ReloadBlockers() const {
  std::vector<ExternalResourceInfo> result;
  heap_.AppendExternalRoots(result);
  scheduler_.AppendExternalResources(result);
  return result;
}

std::expected<void, std::vector<ExternalResourceInfo>> Isolate::TryStop() {
  auto blockers = ReloadBlockers();
  if (!blockers.empty()) {
    return std::unexpected(std::move(blockers));
  }
  native_control_.RequestStop();
  native_dispatch_.Close();
  scheduler_.Shutdown();
  heap_.Collect();
  return {};
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
  if (options.is_enum && options.nominal != nullptr && options.nominal->fixed_layout_ != nullptr) {
    if (native_layout_transactions_ == 0) {
      native_metadata_.classes.erase(options.nominal->fixed_layout_);
    }
    return options.nominal->fixed_layout_;
  }
  std::vector<std::uint32_t> references(reference_fields.begin(), reference_fields.end());
  std::ranges::sort(references);
  const auto duplicates = std::ranges::unique(references);
  references.erase(duplicates.begin(), duplicates.end());
  std::erase_if(references,
                [field_count](std::uint32_t index) -> bool { return index >= field_count; });

  std::unique_ptr<ClassLayout> layout(new ClassLayout(field_count, std::move(references)));
  ClassLayout* result = layout.get();
  result->callable_ = options.callable;
  auto* nominal = options.nominal;
  if (nominal == nullptr) {
    auto kind = NominalKind::Class;
    if (options.is_enum) {
      kind = NominalKind::Enum;
    } else if (options.callable) {
      kind = NominalKind::Callable;
    }
    nominal = CreateNominalType({
        .kind = kind,
        .interfaces = std::move(options.interfaces),
        .declaration = {},
    });
  }
  result->nominal_ = nominal;
  result->storage_ = options.storage;
  result->revision_ = options.revision;
  result->field_declarations_ = std::move(options.field_declarations);
  const auto declarations = result->field_declarations();
  for (std::size_t index = 0; index < declarations.size(); ++index) {
    const auto field = declarations.subspan(index).front();
    if (field != DeclarationId{}) {
      result->field_indices_.emplace(field, static_cast<std::uint32_t>(index));
    }
  }
  if (options.storage == ClassStorage::Inline && nominal->fixed_layout_ == nullptr) {
    // 名义身份由本 Isolate 创建；仅在它的第一个固定布局建立时绑定分配描述。
    nominal->fixed_layout_ = result;
  }
  result->dispatch_targets_ = std::move(options.dispatch_targets);
  result->callable_type_ = options.callable_type;
  class_layouts_.push_back(std::move(layout));
  return result;
}

NominalType* Isolate::CreateNominalType(NominalTypeOptions options) {
  std::unique_ptr<NominalType> identity(new NominalType(std::move(options)));
  auto* result = identity.get();
  nominal_types_.push_back(std::move(identity));
  return result;
}

const ArrayLayout* Isolate::CreateArrayLayout(ArrayLayout layout) {
  if (layout.element.array != nullptr) {
    layout.element.object_kind = ObjectKind::Array;
  }
  const auto* result = InternLayout(layout, array_layout_index_, array_layouts_);
  if (native_layout_transactions_ == 0) {
    native_metadata_.arrays.erase(result);
  }
  return result;
}

const CollectionLayout* Isolate::CreateCollectionLayout(CollectionLayout layout) {
  const auto* result = InternLayout(layout, collection_layout_index_, collection_layouts_);
  if (native_layout_transactions_ == 0) {
    native_metadata_.collections.erase(result);
  }
  return result;
}

const TypeSignature* Isolate::CreateTypeSignature(TypeSignature signature) {
  const auto* result = InternLayout(std::move(signature), type_signature_index_, type_signatures_);
  if (native_layout_transactions_ == 0) {
    native_metadata_.signatures.erase(result);
  }
  return result;
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
  LayoutCounts counts{
      .classes = class_layouts_.size(),
      .arrays = array_layouts_.size(),
      .collections = collection_layouts_.size(),
      .signatures = type_signatures_.size(),
      .interfaces = interface_types_.size(),
      .code_identities = code_identities_.size(),
      .nominal_types = nominal_types_.size(),
  };
  heap_.AccumulateNativeLayouts(counts);
  return counts;
}

}  // namespace vela::runtime
