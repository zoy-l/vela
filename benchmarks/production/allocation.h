#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

#include <absl/container/flat_hash_map.h>

namespace vela::benchmarks::production {

struct AllocationStatistics final {
  std::uint64_t allocations = 0;
  std::uint64_t deallocations = 0;
  std::uint64_t bytes = 0;
  std::uint64_t freed_bytes = 0;
  std::uint64_t live_bytes = 0;
  std::uint64_t peak_bytes = 0;
};

class AllocationLedger final {
 public:
  void Allocate(std::size_t bytes) noexcept;
  void Deallocate(std::size_t bytes) noexcept;
  void ResetActivity() noexcept;
  void* AllocateRaw(std::size_t bytes);
  void FreeRaw(void* pointer) noexcept;
  void* ReallocateRaw(void* pointer, std::size_t old_size, std::size_t bytes);
  [[nodiscard]] const AllocationStatistics& statistics() const noexcept { return statistics_; }
  [[nodiscard]] bool IsValid() const noexcept { return valid_; }

 private:
  AllocationStatistics statistics_;
  bool valid_ = true;
  absl::flat_hash_map<void*, std::size_t> raw_sizes_;
};

AllocationLedger* ActiveAllocationLedger() noexcept;
void* AllocateCounted(void* context, std::size_t bytes);
void FreeCounted(void* context, void* pointer) noexcept;
void* ReallocateCounted(void* context, void* pointer, std::size_t old_size, std::size_t bytes);

// 仅分配统计进程启用；普通吞吐和延迟使用 std::allocator，不读取此线程状态。
class AllocationScope final {
 public:
  explicit AllocationScope(AllocationLedger& ledger) noexcept;
  ~AllocationScope();
  AllocationScope(const AllocationScope&) = delete;
  AllocationScope& operator=(const AllocationScope&) = delete;
  AllocationScope(AllocationScope&&) = delete;
  AllocationScope& operator=(AllocationScope&&) = delete;

 private:
  AllocationLedger* previous_;
};

template <class T>
class CountingAllocator final {
 public:
  using value_type = T;
  template <class U>
  struct rebind {
    using other = CountingAllocator<U>;
  };
  CountingAllocator() noexcept : ledger_(ActiveAllocationLedger()) {}
  template <class U>
  explicit CountingAllocator(const CountingAllocator<U>& other) noexcept
      : ledger_(other.ledger()) {}
  [[nodiscard]] T* allocate(std::size_t count) {
    auto* value = std::allocator<T>{}.allocate(count);
    ledger_->Allocate(count * sizeof(T));
    return value;
  }
  void deallocate(T* value, std::size_t count) noexcept {
    ledger_->Deallocate(count * sizeof(T));
    std::allocator<T>{}.deallocate(value, count);
  }
  [[nodiscard]] AllocationLedger* ledger() const noexcept { return ledger_; }
  template <class U>
  bool operator==(const CountingAllocator<U>& other) const noexcept {
    return ledger_ == other.ledger();
  }

 private:
  AllocationLedger* ledger_;
};

template <class T, bool Counted>
using Allocator = std::conditional_t<Counted, CountingAllocator<T>, std::allocator<T>>;

}  // namespace vela::benchmarks::production
