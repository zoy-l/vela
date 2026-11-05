#include <vela/handles.h>

#include <array>
#include <cassert>
#include <cstddef>
#include <expected>
#include <memory>
#include <span>

#include "base/execution_status.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/value/value.h"

namespace vela {

struct HandleScope::Block final {
  static constexpr std::size_t kCapacity = 32;
  explicit Block(runtime::Heap& heap, Block* previous) noexcept
      : roots(heap, values.data(), values.size()), next(previous) {}
  std::array<runtime::Value, kCapacity> values;
  runtime::RootRange roots;
  std::size_t count = 0;
  Block* next;
};

HandleScope::HandleScope(runtime::Isolate& isolate) noexcept
    : isolate_(&isolate),
      inline_roots_(isolate.heap(), inline_values_.data(), inline_values_.size()) {
  assert(isolate.scheduler().IsOwnerThread());
}

HandleScope::~HandleScope() {
  assert(isolate_->scheduler().IsOwnerThread());
  auto& heap = isolate_->heap();
  auto* block = blocks_;
  while (block != nullptr) {
    auto* next = block->next;
    block->~Block();
    heap.FreeNative(block);
    block = next;
  }
}

std::expected<runtime::Value*, ExecutionStatus> HandleScope::Store(runtime::Value value) noexcept {
  auto& heap = isolate_->heap();
  runtime::Value* slot = nullptr;
  if (inline_count_ < kInlineCapacity) {
    slot = &inline_values_[inline_count_++];
  } else {
    if (blocks_ == nullptr || blocks_->count == Block::kCapacity) {
      const runtime::RootRange source_root(heap, &value, 1, runtime::RootOwner::Runtime);
      auto* memory = heap.AllocateNative(sizeof(Block));
      if (memory == nullptr) {
        return std::unexpected(ExecutionStatus::OutOfMemory);
      }
      blocks_ = std::construct_at(static_cast<Block*>(memory), heap, blocks_);
    }
    slot = &blocks_->values[blocks_->count++];
  }
  *slot = value;
  heap.WriteRootBarrier(std::span(slot, 1));
  return slot;
}

}  // namespace vela
