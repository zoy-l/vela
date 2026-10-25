#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

#include "base/execution_status.h"
#include "runtime/exception.h"
#include "runtime/object.h"
#include "runtime/register_storage.h"
#include "runtime/value.h"

namespace vela::runtime {

class Heap;

class RootRange final {
 public:
  RootRange() noexcept = default;
  RootRange(Heap& heap, const Value* values, std::size_t count) noexcept;
  RootRange(Heap& heap, std::span<StringObject* const> strings) noexcept;
  ~RootRange();

  RootRange(const RootRange&) = delete;
  auto operator=(const RootRange&) -> RootRange& = delete;
  RootRange(RootRange&&) = delete;
  auto operator=(RootRange&&) -> RootRange& = delete;

  void Attach(Heap& heap, const Value* values, std::size_t count) noexcept;
  void Attach(Heap& heap, RegisterView registers) noexcept;
  void Detach() noexcept;

 private:
  friend class Heap;

  enum class Kind : std::uint8_t { Values, Strings, Registers };

  Heap* heap_ = nullptr;
  const void* data_ = nullptr;
  const ValueKind* register_kinds_ = nullptr;
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
  OutOfMemory,
};

enum class StringBuilderError : std::uint8_t { NotBuilder, OutOfMemory };
enum class ExpectedAlternative : std::uint8_t { Ok, Err };
enum class ExpectedAccessError : std::uint8_t { NotExpected, InactiveAlternative };
enum class CollectionError : std::uint8_t { NotCollection, TypeMismatch, OutOfMemory };
enum class CollectionInsert : std::uint8_t { Inserted, Updated };
enum class CollectionRemove : std::uint8_t { Removed, Missing };
enum class CollectionProjection : std::uint8_t { Keys, Values };

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
  [[nodiscard]] auto AllocateExpected(ClassLayout& layout,
                                      Value payload,
                                      ExpectedAlternative alternative) noexcept -> HeapObject*;
  [[nodiscard]] static auto ExpectedAlternativeOf(const HeapObject& object) noexcept
      -> std::expected<ExpectedAlternative, ExpectedAccessError>;
  [[nodiscard]] static auto ReadExpectedValue(const HeapObject& object,
                                              ExpectedAlternative alternative) noexcept
      -> std::expected<Value, ExpectedAccessError>;
  [[nodiscard]] auto AllocateCollection(const CollectionLayout& layout) noexcept -> HeapObject*;
  // 扩容可能触发 GC；原生调用者须在调用期间保根 object、key 和 value。
  [[nodiscard]] auto InsertCollection(HeapObject& object,
                                      Value key,
                                      Value value = Value::Null()) noexcept
      -> std::expected<CollectionInsert, CollectionError>;
  [[nodiscard]] static auto HasCollectionKey(const HeapObject& object, Value key) noexcept
      -> std::expected<bool, CollectionError>;
  [[nodiscard]] static auto GetCollectionValue(const HeapObject& object, Value key) noexcept
      -> std::expected<Value, CollectionError>;
  [[nodiscard]] static auto RemoveCollection(HeapObject& object, Value key) noexcept
      -> std::expected<CollectionRemove, CollectionError>;
  [[nodiscard]] static auto ClearCollection(HeapObject& object) noexcept
      -> std::expected<void, CollectionError>;
  [[nodiscard]] static auto CollectionLength(const HeapObject& object) noexcept
      -> std::expected<std::uint32_t, CollectionError>;
  [[nodiscard]] auto CopyCollection(const HeapObject& object,
                                    HeapObject& array,
                                    CollectionProjection projection) noexcept
      -> std::expected<void, CollectionError>;
  // 输入必须是合法 UTF-8；堆字符串须由调用者保根，分配可触发 GC。
  [[nodiscard]] auto AllocateString(std::string_view first, std::string_view second = {}) noexcept
      -> StringObject*;
  // parts 不含空指针；所有输入及 separator 的堆存储须保根，最终结果只分配一次。
  [[nodiscard]] auto JoinStrings(std::span<StringObject* const> parts,
                                 std::string_view separator = {}) noexcept -> StringObject*;
  // 返回视图借用数组的连续存储；跨分配使用时必须保持数组可达。
  [[nodiscard]] static auto StringElements(const HeapObject& array) noexcept
      -> std::expected<std::span<StringObject* const>, ArrayAccessError>;
  [[nodiscard]] static auto IntegerElements(HeapObject& array) noexcept
      -> std::expected<std::span<std::int64_t>, ArrayAccessError>;
  [[nodiscard]] static auto ArrayLength(const HeapObject& array) noexcept
      -> std::expected<std::uint32_t, ArrayAccessError>;
  [[nodiscard]] auto SliceArray(HeapObject& array, std::uint32_t start, std::uint32_t end) noexcept
      -> std::expected<HeapObject*, ArrayAccessError>;
  [[nodiscard]] auto FillArray(HeapObject& array, Value value) noexcept
      -> std::expected<void, ArrayAccessError>;
  [[nodiscard]] static auto ReverseArray(HeapObject& array) noexcept
      -> std::expected<void, ArrayAccessError>;
  [[nodiscard]] auto AllocateStringBuilder() noexcept -> HeapObject*;
  [[nodiscard]] auto AllocateException(HeapObject& payload,
                                       ExecutionStatus status,
                                       std::span<const TracePosition> trace,
                                       std::string_view message) noexcept -> HeapObject*;
  // builder 与 text 的堆存储须在扩容期间保根；text 必须是合法 UTF-8。
  [[nodiscard]] auto AppendString(HeapObject& builder, std::string_view text) noexcept
      -> std::expected<void, StringBuilderError>;
  // builder 必须来自本 Heap 的 AllocateStringBuilder，且调用期间保根。
  [[nodiscard]] auto BuildString(const HeapObject& builder) noexcept -> StringObject*;
  [[nodiscard]] static auto IsStringBuilder(const HeapObject& object) noexcept -> bool;

  [[nodiscard]] static auto GetField(HeapObject& object, std::uint32_t index) noexcept -> Value* {
    if (object.layout == nullptr || index >= object.layout->field_count()) {
      return nullptr;
    }
    return std::span(ObjectFields(&object), object.layout->field_count()).subspan(index).data();
  }
  [[nodiscard]] static auto ClassOf(const HeapObject& object) noexcept -> const ClassLayout*;
  [[nodiscard]] static auto IsValueOfType(Value value, const TypeConstraint& type) noexcept -> bool;
  [[nodiscard]] static auto GetField(const HeapObject& object, std::uint32_t index) noexcept
      -> const Value* {
    if (object.layout == nullptr || index >= object.layout->field_count()) {
      return nullptr;
    }
    return std::span(ObjectFields(&object), object.layout->field_count()).subspan(index).data();
  }
  [[nodiscard]] auto SetField(HeapObject& object, std::uint32_t index, Value value) noexcept
      -> bool {
    auto* field = GetField(object, index);
    if (field == nullptr) {
      return false;
    }
    *field = value;
    if (value.IsObject() || value.IsString()) {
      WriteBarrier(object, value);
    }
    return true;
  }
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
  void MarkCollection(HeapObject& object) noexcept;
  void ReleaseCollection(HeapObject& object) noexcept;
  auto GrowCollection(HeapObject& object) noexcept -> std::expected<void, CollectionError>;

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
