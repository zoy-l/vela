#include "runtime/heap/host_root.h"
#include <optional>
#include "runtime/isolate/external_resources.h"

#include <cassert>
#include <expected>
#include <memory>
#include <type_traits>

#include "base/execution_status.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/host_callback.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {
namespace {

bool HasScalarCallbackSignature(const ClassLayout* layout) {
  const auto* signature = layout != nullptr ? layout->callable_type() : nullptr;
  if (signature == nullptr || signature->parameters.size() != 1 || !signature->result) {
    return false;
  }
  const auto& argument = signature->parameters.front().type;
  const auto kind = argument.kind;
  return !argument.nullable && !argument.IsDynamic() && argument == *signature->result &&
         (kind == ValueKind::Integer || kind == ValueKind::Double || kind == ValueKind::Boolean);
}

}  // namespace

HostRoot::Storage::Storage(Isolate& owner, Value source) noexcept
    : isolate(&owner),
      value(source),
      root(owner.heap(), &value, 1, ExternalResourceKind::Persistent) {}

std::expected<HostRoot, ExecutionStatus> HostRoot::Create(Isolate& isolate, Value value) noexcept {
  assert(isolate.scheduler().IsOwnerThread());
  auto& heap = isolate.heap();
  const RootRange source_root(heap, &value, 1, std::nullopt);
  const auto* object = value.AsObjectOrNull();
  const auto* layout = object != nullptr ? Heap::ClassOf(*object) : nullptr;
  const bool has_callback = HasScalarCallbackSignature(layout);
  using Callback = ::vela::internal::BoundScalarCallback;
  const auto bytes = sizeof(Storage) + (has_callback ? sizeof(Callback) : 0);
  auto* memory = heap.AllocateNative(bytes);
  if (memory == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  auto* storage = std::construct_at(static_cast<Storage*>(memory), isolate, value);
  if (has_callback) {
    // 描述随同不可变根一次分配，普通根不承担缓存空间；描述本身不拥有额外资源。
    static_assert(std::is_trivially_destructible_v<Callback>);
    std::construct_at(StorageAfter<Callback>(*storage), Callback::Bind(isolate, value));
  }
  return HostRoot(*storage);
}

void HostRoot::Destroy(Storage& storage) noexcept {
  auto& isolate = *storage.isolate;
  storage.~Storage();
  isolate.heap().FreeNative(&storage);
}

bool HostRoot::IsOwnerThread() const noexcept {
  return storage_->isolate->scheduler().IsOwnerThread();
}

}  // namespace vela::runtime
