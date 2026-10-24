#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "runtime/object.h"

namespace vela::runtime {

class Heap;

class RootRange final {
 public:
  RootRange(Heap& heap, Value* values, std::size_t count) noexcept;
  ~RootRange();

  RootRange(const RootRange&) = delete;
  RootRange& operator=(const RootRange&) = delete;
  RootRange(RootRange&&) = delete;
  RootRange& operator=(RootRange&&) = delete;

 private:
  friend class Heap;

  Heap* heap_ = nullptr;
  Value* values_ = nullptr;
  std::size_t count_ = 0;
  RootRange* next_ = nullptr;
  RootRange** previous_link_ = nullptr;
};

enum class GcPhase : std::uint8_t {
  Idle,
  Mark,
  Sweep,
};

class Heap final {
 public:
  explicit Heap(std::size_t gc_threshold = 64 * 1024) noexcept;
  ~Heap();

  Heap(const Heap&) = delete;
  Heap& operator=(const Heap&) = delete;

  [[nodiscard]] HeapObject* AllocateInstance(ClassLayout& layout,
                                             std::span<const Value> initial_fields = {}) noexcept;

  [[nodiscard]] Value* GetField(HeapObject& object, std::uint32_t index) noexcept;
  [[nodiscard]] const Value* GetField(const HeapObject& object, std::uint32_t index) const noexcept;
  [[nodiscard]] bool SetField(HeapObject& object, std::uint32_t index, Value value) noexcept;

  void StartCollection() noexcept;
  void Step(std::size_t work_budget) noexcept;
  void FinishCollection() noexcept;
  void Collect() noexcept;

  void SetGcThreshold(std::size_t threshold) noexcept;

  [[nodiscard]] GcPhase phase() const noexcept { return phase_; }
  [[nodiscard]] bool IsCollecting() const noexcept { return phase_ != GcPhase::Idle; }
  [[nodiscard]] std::size_t object_count() const noexcept { return object_count_; }
  [[nodiscard]] std::size_t allocated_bytes() const noexcept { return allocated_bytes_; }
  [[nodiscard]] std::size_t committed_bytes() const noexcept { return committed_bytes_; }

 private:
  friend class RootRange;

  struct FreeBlock {
    FreeBlock* next = nullptr;
  };

  static constexpr std::array<std::size_t, 8> kSizeClasses = {32,  64,   128,  256,
                                                              512, 1024, 2048, 4096};
  static constexpr std::size_t kAutomaticWork = 16;

  void RegisterRoot(RootRange& root) noexcept;
  void UnregisterRoot(RootRange& root) noexcept;
  void MarkRoots() noexcept;
  void MarkValue(Value value) noexcept;
  void MarkObject(HeapObject& object) noexcept;
  void ProcessGrayObject(HeapObject& object) noexcept;
  void DrainGray() noexcept;
  void WriteBarrier(HeapObject& source, Value value) noexcept;

  [[nodiscard]] HeapObject* AllocateRaw(std::size_t requested_size) noexcept;
  void ReleaseRaw(HeapObject& object) noexcept;
  [[nodiscard]] std::size_t SelectBlockSize(std::size_t requested_size) const noexcept;
  [[nodiscard]] std::size_t SizeClassIndex(std::size_t block_size) const noexcept;
  [[nodiscard]] bool ShouldCollect(std::size_t next_size) const noexcept;
  void MaybeCollect(std::size_t next_size) noexcept;
  void FinishSweep() noexcept;
  void DestroyAll() noexcept;

  HeapObject* objects_ = nullptr;
  HeapObject* gray_ = nullptr;
  HeapObject** sweep_link_ = nullptr;
  RootRange* roots_ = nullptr;
  std::array<FreeBlock*, kSizeClasses.size()> free_blocks_{};

  std::size_t object_count_ = 0;
  std::size_t allocated_bytes_ = 0;
  std::size_t committed_bytes_ = 0;
  std::size_t gc_threshold_ = 0;
  std::size_t minimum_gc_threshold_ = 0;
  std::uint8_t current_mark_bit_ = 1;
  GcPhase phase_ = GcPhase::Idle;
};

}  // namespace vela::runtime
