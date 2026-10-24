#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

#include "runtime/object.h"
#include "runtime/value.h"

namespace vela::runtime {

class Heap;

class RootRange final {
 public:
  RootRange() noexcept = default;
  RootRange(Heap& heap, Value* values, std::size_t count) noexcept;
  RootRange(Heap& heap, std::span<StringObject* const> strings) noexcept;
  ~RootRange();

  RootRange(const RootRange&) = delete;
  auto operator=(const RootRange&) -> RootRange& = delete;
  RootRange(RootRange&&) = delete;
  auto operator=(RootRange&&) -> RootRange& = delete;

  void Attach(Heap& heap, Value* values, std::size_t count) noexcept;
  void Detach() noexcept;

 private:
  friend class Heap;

  enum class Kind : std::uint8_t { Values, Strings };

  Heap* heap_ = nullptr;
  const void* data_ = nullptr;
  std::size_t count_ = 0;
  RootRange* next_ = nullptr;
  RootRange** previous_link_ = nullptr;
  Kind kind_ = Kind::Values;
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

enum class StringBuilderError : std::uint8_t { NotBuilder, OutOfMemory };

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
  // 输入必须是合法 UTF-8；堆字符串须由调用者保根，分配可触发 GC。
  [[nodiscard]] auto AllocateString(std::string_view first, std::string_view second = {}) noexcept
      -> StringObject*;
  // parts 不含空指针；所有输入及 separator 的堆存储须保根，最终结果只分配一次。
  [[nodiscard]] auto JoinStrings(std::span<StringObject* const> parts,
                                 std::string_view separator = {}) noexcept -> StringObject*;
  // 返回视图借用数组的连续存储；跨分配使用时必须保持数组可达。
  [[nodiscard]] static auto StringElements(const HeapObject& array) noexcept
      -> std::expected<std::span<StringObject* const>, ArrayAccessError>;
  [[nodiscard]] auto AllocateStringBuilder() noexcept -> HeapObject*;
  // builder 与 text 的堆存储须在扩容期间保根；text 必须是合法 UTF-8。
  [[nodiscard]] auto AppendString(HeapObject& builder, std::string_view text) noexcept
      -> std::expected<void, StringBuilderError>;
  // builder 必须来自本 Heap 的 AllocateStringBuilder，且调用期间保根。
  [[nodiscard]] auto BuildString(const HeapObject& builder) noexcept -> StringObject*;
  [[nodiscard]] static auto IsStringBuilder(const HeapObject& object) noexcept -> bool;

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
  void MarkRoot(const RootRange& root) noexcept;
  void MarkValue(Value value) noexcept;
  void MarkObject(HeapObject& object) noexcept;
  void ProcessGrayObject(HeapObject& object) noexcept;
  void DrainGray() noexcept;
  void WriteBarrier(HeapObject& source, Value value) noexcept;

  struct Allocation final {
    void* memory = nullptr;
    std::uint32_t size = 0;
  };

  [[nodiscard]] auto AllocateRaw(std::size_t requested_size) noexcept -> Allocation;
  void Publish(HeapObject& object, std::uint32_t allocation_size) noexcept;
  void ReleaseRaw(HeapObject& object) noexcept;
  [[nodiscard]] static auto SelectBlockSize(std::size_t requested_size) noexcept -> std::size_t;
  [[nodiscard]] static auto SizeClassIndex(std::size_t block_size) noexcept -> std::size_t;
  [[nodiscard]] auto ShouldCollect(std::size_t next_size) const noexcept -> bool;
  void MaybeCollect(std::size_t next_size) noexcept;
  void FinishSweep() noexcept;
  void DestroyAll() noexcept;
  [[nodiscard]] auto AllocateStringStorage(std::size_t length) noexcept -> StringObject*;
  void ReleaseStringBuilder(HeapObject& object) noexcept;

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
