#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>

#include "runtime/object.h"

namespace vela::runtime {

class Heap;

class RootRange final {
 public:
  RootRange() noexcept = default;
  RootRange(Heap& heap, Value* values, std::size_t count) noexcept;
  ~RootRange();

  RootRange(const RootRange&) = delete;
  auto operator=(const RootRange&) -> RootRange& = delete;
  RootRange(RootRange&&) = delete;
  auto operator=(RootRange&&) -> RootRange& = delete;

  void Attach(Heap& heap, Value* values, std::size_t count) noexcept;
  void Detach() noexcept;

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

inline constexpr std::size_t kDefaultGcThreshold = 65536;

enum class ArrayAccessError : std::uint8_t {
  NotArray,
  OutOfBounds,
  ElementTypeMismatch,
};

class Heap final {
 public:
  explicit Heap(std::size_t gc_threshold = kDefaultGcThreshold) noexcept;
  ~Heap();

  Heap(Heap&&) = delete;
  auto operator=(Heap&&) -> Heap& = delete;
  Heap(const Heap&) = delete;
  auto operator=(const Heap&) -> Heap& = delete;

  [[nodiscard]] auto AllocateInstance(ClassLayout& layout,
                                      std::span<const Value> initial_fields = {}) noexcept
      -> HeapObject*;
  [[nodiscard]] auto AllocateArray(const ArrayLayout& layout, std::size_t length) noexcept
      -> HeapObject*;

  [[nodiscard]] static auto GetField(HeapObject& object, std::uint32_t index) noexcept -> Value*;
  [[nodiscard]] static auto GetField(const HeapObject& object, std::uint32_t index) noexcept
      -> const Value*;
  [[nodiscard]] auto SetField(HeapObject& object, std::uint32_t index, Value value) noexcept
      -> bool;
  [[nodiscard]] static auto GetElement(const HeapObject& object, std::uint32_t index) noexcept
      -> std::expected<Value, ArrayAccessError>;
  [[nodiscard]] auto SetElement(HeapObject& object, std::uint32_t index, Value value) noexcept
      -> std::expected<void, ArrayAccessError>;

  void StartCollection() noexcept;
  void Step(std::size_t work_budget) noexcept;
  void FinishCollection() noexcept;
  void Collect() noexcept;

  void SetGcThreshold(std::size_t threshold) noexcept;

  [[nodiscard]] auto phase() const noexcept -> GcPhase { return phase_; }
  [[nodiscard]] auto IsCollecting() const noexcept -> bool { return phase_ != GcPhase::Idle; }
  [[nodiscard]] auto object_count() const noexcept -> std::size_t { return object_count_; }
  [[nodiscard]] auto allocated_bytes() const noexcept -> std::size_t { return allocated_bytes_; }
  [[nodiscard]] auto committed_bytes() const noexcept -> std::size_t { return committed_bytes_; }

 private:
  friend class RootRange;

  static constexpr auto kSizeClasses =
      std::to_array<std::size_t>({32, 64, 128, 256, 512, 1024, 2048, 4096});
  static constexpr std::size_t kAutomaticWork = 16;

  void RegisterRoot(RootRange& root) noexcept;
  static void UnregisterRoot(RootRange& root) noexcept;
  void MarkRoots() noexcept;
  void MarkValue(Value value) noexcept;
  void MarkObject(HeapObject& object) noexcept;
  void ProcessGrayObject(HeapObject& object) noexcept;
  void DrainGray() noexcept;
  void WriteBarrier(HeapObject& source, Value value) noexcept;

  [[nodiscard]] auto AllocateRaw(std::size_t requested_size) noexcept -> HeapObject*;
  void ReleaseRaw(HeapObject& object) noexcept;
  [[nodiscard]] static auto SelectBlockSize(std::size_t requested_size) noexcept -> std::size_t;
  [[nodiscard]] static auto SizeClassIndex(std::size_t block_size) noexcept -> std::size_t;
  [[nodiscard]] auto ShouldCollect(std::size_t next_size) const noexcept -> bool;
  void MaybeCollect(std::size_t next_size) noexcept;
  void FinishSweep() noexcept;
  void DestroyAll() noexcept;

  HeapObject* objects_ = nullptr;
  HeapObject* gray_ = nullptr;
  HeapObject** sweep_link_ = nullptr;
  RootRange* roots_ = nullptr;
  std::array<HeapObject*, kSizeClasses.size()> free_blocks_{};

  std::size_t object_count_ = 0;
  std::size_t allocated_bytes_ = 0;
  std::size_t committed_bytes_ = 0;
  std::size_t gc_threshold_ = 0;
  std::size_t minimum_gc_threshold_ = 0;
  std::uint8_t current_mark_bit_ = 1;
  GcPhase phase_ = GcPhase::Idle;
};

}  // namespace vela::runtime
