#include "runtime/native/host_type.h"
#include <optional>
#include "runtime/isolate/external_resources.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <utility>

#include <absl/container/flat_hash_map.h>

#include "base/execution_status.h"
#include "base/layout_description.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/isolate/program_layouts.h"
#include "runtime/native/native_dispatch.h"
#include "runtime/native/native_image.h"
#include "runtime/native/native_instance.h"
#include "runtime/native/native_type.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {
namespace {

std::expected<LayoutCache, ExecutionStatus> BindNominals(const NativeInstance& instance,
                                                         const layout::Program& description) {
  LayoutCache cache;
  if (description.classes.empty() && description.interfaces.empty()) {
    return cache;
  }
  const auto classes = instance.image().classes.values();
  const auto& layouts = instance.layouts();
  absl::flat_hash_map<std::string_view, std::size_t> class_indices;
  for (std::size_t index = 0; index < classes.size(); ++index) {
    class_indices.emplace(classes[index].name.text(), index);
  }
  for (const auto& nominal : description.classes) {
    if (nominal.kind == layout::Class::Kind::Host) {
      if (instance.isolate().FindNativeClass(nominal.native_key) == nullptr) {
        return std::unexpected(ExecutionStatus::InvalidEntry);
      }
      continue;
    }
    const auto found = class_indices.find(nominal.name);
    if (found == class_indices.end()) {
      return std::unexpected(ExecutionStatus::InvalidEntry);
    }
    const auto index = found->second;
    if (classes[index].kind != static_cast<std::uint32_t>(nominal.kind)) {
      return std::unexpected(ExecutionStatus::InvalidEntry);
    }
    cache.classes.emplace(nominal.name, layouts.classes[index]);
  }
  const auto interfaces = instance.image().interfaces.values();
  absl::flat_hash_map<std::string_view, const InterfaceType*> interface_types;
  for (std::size_t index = 0; index < interfaces.size(); ++index) {
    interface_types.emplace(interfaces[index].text(), layouts.interfaces[index]);
  }
  for (const auto& name : description.interfaces) {
    const auto found = interface_types.find(name);
    if (found == interface_types.end()) {
      return std::unexpected(ExecutionStatus::InvalidEntry);
    }
    cache.interfaces.emplace(name, found->second);
  }
  return cache;
}

}  // namespace

std::expected<TypeConstraint, ExecutionStatus> ResolveInstanceNativeType(NativeInstance& instance,
                                                                         NativeType description) {
  if (!description.type) {
    return std::unexpected(ExecutionStatus::InvalidEntry);
  }
  auto cache = BindNominals(instance, description.layouts());
  if (!cache) {
    return std::unexpected(cache.error());
  }
  return InstantiateNativeType(instance.isolate(), description, *cache);
}

HostType::Storage::Storage(Isolate& owner,
                           TypeConstraint contract,
                           Value state,
                           NativeDispatch* session) noexcept
    : isolate(&owner),
      constraint(contract),
      anchor(state),
      dispatch(session),
      program_root(owner.heap(), &anchor, 1, std::nullopt),
      type_root(owner.heap(), std::span(&constraint, 1), ExternalResourceKind::Type) {}

std::expected<HostType, ExecutionStatus> HostType::Create(Isolate& isolate,
                                                          TypeConstraint constraint,
                                                          Value anchor,
                                                          NativeDispatch* dispatch) {
  assert(isolate.scheduler().IsOwnerThread());
  auto& heap = isolate.heap();
  const RootRange program_root(heap, &anchor, 1, std::nullopt);
  const RootRange type_root(heap, std::span(&constraint, 1));
  auto* memory = heap.AllocateNative(sizeof(Storage));
  if (memory == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  return HostType(*::new (memory) Storage(isolate, constraint, anchor, dispatch));
}

std::expected<HostType, ExecutionStatus> HostType::Bind(Isolate& isolate, NativeType description) {
  const auto constraint = ResolveBuiltinNativeType(isolate, description);
  if (!constraint) {
    return std::unexpected(constraint.error());
  }
  return Create(isolate, *constraint, Value::Null(), nullptr);
}

std::expected<HostType, ExecutionStatus> HostType::Bind(NativeInstance& instance,
                                                        NativeType description) {
  const auto constraint = ResolveInstanceNativeType(instance, description);
  if (!constraint) {
    return std::unexpected(constraint.error());
  }
  return Bind(instance, *constraint);
}

std::expected<HostType, ExecutionStatus> HostType::Bind(NativeInstance& instance,
                                                        TypeConstraint constraint) {
  auto& isolate = instance.isolate();
  return Create(isolate, constraint, instance.context_anchor(), instance.dispatch());
}

std::expected<HostType, ExecutionStatus> HostType::WithConstraint(TypeConstraint constraint) const {
  return Create(isolate(), constraint, storage_->anchor, storage_->dispatch);
}

std::expected<HostType, ExecutionStatus> HostType::Bind(NativeDispatch& dispatch,
                                                        NativeType description) {
  const auto state = dispatch.BorrowCurrentState();
  if (!state) {
    return std::unexpected(state.error());
  }
  auto instance = NativeInstance::Borrow(**state);
  return Bind(instance, description);
}

HostType::~HostType() {
  Reset();
}

HostType::HostType(const HostType& other) noexcept : storage_(other.storage_) {
  Retain();
}

HostType& HostType::operator=(const HostType& other) noexcept {
  if (this != &other) {
    Reset();
    storage_ = other.storage_;
    Retain();
  }
  return *this;
}

HostType::HostType(HostType&& other) noexcept : storage_(std::exchange(other.storage_, nullptr)) {}

HostType& HostType::operator=(HostType&& other) noexcept {
  if (this != &other) {
    Reset();
    storage_ = std::exchange(other.storage_, nullptr);
  }
  return *this;
}

void HostType::Retain() noexcept {
  if (storage_ != nullptr) {
    assert(storage_->isolate->scheduler().IsOwnerThread());
    ++storage_->references;
  }
}

void HostType::Reset() noexcept {
  auto* storage = std::exchange(storage_, nullptr);
  if (storage == nullptr) {
    return;
  }
  auto& isolate = *storage->isolate;
  assert(isolate.scheduler().IsOwnerThread());
  if (--storage->references == 0) {
    storage->~Storage();
    isolate.heap().FreeNative(storage);
  }
}

Isolate& HostType::isolate() const noexcept {
  assert(storage_ != nullptr);
  return *storage_->isolate;
}

TypeConstraint HostType::constraint() const noexcept {
  assert(isolate().scheduler().IsOwnerThread());
  return storage_->constraint;
}

std::expected<HeapObject*, ExecutionStatus> HostType::program() const noexcept {
  assert(isolate().scheduler().IsOwnerThread());
  if (const auto status = isolate().native_control().Poll(); status != ExecutionStatus::Returned) {
    return std::unexpected(status);
  }
  if (const auto* dispatch = storage_->dispatch) {
    return dispatch->BorrowCurrentState();
  }
  if (storage_->anchor.IsNull()) {
    return std::unexpected(ExecutionStatus::InvalidEntry);
  }
  return storage_->anchor.AsObject();
}

const void* HostType::context_identity() const noexcept {
  if (storage_->dispatch != nullptr) {
    return storage_->dispatch;
  }
  return storage_->anchor.IsNull() ? nullptr : storage_->anchor.AsObject();
}

}  // namespace vela::runtime
