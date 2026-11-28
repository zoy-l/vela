#include "benchmarks/production/allocation.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace vela::benchmarks::production {

static AllocationLedger*& CurrentLedger() noexcept {
  static thread_local AllocationLedger* ledger = nullptr;
  return ledger;
}

void* AllocationLedger::AllocateRaw(std::size_t bytes) {
  auto* pointer = ::operator new(bytes);
  raw_sizes_.emplace(pointer, bytes);
  Allocate(bytes);
  return pointer;
}

void AllocationLedger::FreeRaw(void* pointer) noexcept {
  if (pointer == nullptr) {
    return;
  }
  const auto found = raw_sizes_.find(pointer);
  Deallocate(found->second);
  raw_sizes_.erase(found);
  ::operator delete(pointer);
}

void* AllocationLedger::ReallocateRaw(void* pointer, std::size_t old_size, std::size_t bytes) {
  auto* result = AllocateRaw(bytes);
  if (pointer != nullptr) {
    std::memcpy(result, pointer, std::min(bytes, old_size));
    FreeRaw(pointer);
  }
  return result;
}

void* AllocateCounted(void* context, std::size_t bytes) {
  return static_cast<AllocationLedger*>(context)->AllocateRaw(bytes);
}

void FreeCounted(void* context, void* pointer) noexcept {
  static_cast<AllocationLedger*>(context)->FreeRaw(pointer);
}

void* ReallocateCounted(void* context, void* pointer, std::size_t old_size, std::size_t bytes) {
  return static_cast<AllocationLedger*>(context)->ReallocateRaw(pointer, old_size, bytes);
}

void AllocationLedger::Allocate(std::size_t bytes) noexcept {
  ++statistics_.allocations;
  statistics_.bytes += bytes;
  statistics_.live_bytes += bytes;
  statistics_.peak_bytes = std::max(statistics_.peak_bytes, statistics_.live_bytes);
}

void AllocationLedger::Deallocate(std::size_t bytes) noexcept {
  if (bytes > statistics_.live_bytes) {
    valid_ = false;
    return;
  }
  ++statistics_.deallocations;
  statistics_.freed_bytes += bytes;
  statistics_.live_bytes -= bytes;
}

void AllocationLedger::ResetActivity() noexcept {
  const auto live = statistics_.live_bytes;
  statistics_ = {.live_bytes = live, .peak_bytes = live};
}

AllocationLedger* ActiveAllocationLedger() noexcept {
  return CurrentLedger();
}

AllocationScope::AllocationScope(AllocationLedger& ledger) noexcept : previous_(CurrentLedger()) {
  CurrentLedger() = &ledger;
}

AllocationScope::~AllocationScope() {
  CurrentLedger() = previous_;
}

}  // namespace vela::benchmarks::production
