#include "runtime/heap/host_root.h"
#include <optional>
#include "runtime/isolate/external_resources.h"

#include <cassert>
#include <cstddef>
#include <expected>
#include <utility>

#include "base/execution_status.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/value/value.h"

namespace vela::runtime {

struct HostRoot::Storage final {
  Storage(Isolate& owner, Value source) noexcept
      : isolate(&owner),
        value(source),
        root(owner.heap(), &value, 1, ExternalResourceKind::Persistent) {}
  Isolate* isolate;
  Value value;
  RootRange root;
  std::size_t references = 1;
};

std::expected<HostRoot, ExecutionStatus> HostRoot::Create(Isolate& isolate, Value value) noexcept {
  assert(isolate.scheduler().IsOwnerThread());
  auto& heap = isolate.heap();
  const RootRange source_root(heap, &value, 1, std::nullopt);
  auto* memory = heap.AllocateNative(sizeof(Storage));
  if (memory == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  return HostRoot(*::new (memory) Storage(isolate, value));
}

HostRoot::~HostRoot() {
  Reset();
}

HostRoot::HostRoot(const HostRoot& other) noexcept : storage_(other.storage_) {
  Retain();
}

HostRoot& HostRoot::operator=(const HostRoot& other) noexcept {
  if (this != &other) {
    Reset();
    storage_ = other.storage_;
    Retain();
  }
  return *this;
}

HostRoot::HostRoot(HostRoot&& other) noexcept : storage_(std::exchange(other.storage_, nullptr)) {}

HostRoot& HostRoot::operator=(HostRoot&& other) noexcept {
  if (this != &other) {
    Reset();
    storage_ = std::exchange(other.storage_, nullptr);
  }
  return *this;
}

void HostRoot::Retain() noexcept {
  if (storage_ != nullptr) {
    assert(storage_->isolate->scheduler().IsOwnerThread());
    ++storage_->references;
  }
}

void HostRoot::Reset() noexcept {
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

Isolate& HostRoot::isolate() const noexcept {
  assert(storage_ != nullptr);
  return *storage_->isolate;
}

Value HostRoot::value() const noexcept {
  assert(storage_ != nullptr);
  assert(storage_->isolate->scheduler().IsOwnerThread());
  return storage_->value;
}

}  // namespace vela::runtime
