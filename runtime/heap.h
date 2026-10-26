#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "base/execution_status.h"
#include "runtime/exception.h"
#include "runtime/future.h"
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
  RootRange& operator=(const RootRange&) = delete;
  RootRange(RootRange&&) = delete;
  RootRange& operator=(RootRange&&) = delete;

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

enum class MutableStringError : std::uint8_t { NotMutableString, OutOfMemory };
enum class StringJoinError : std::uint8_t { TypeMismatch, OutOfMemory };

enum class StringCase : std::uint8_t { Lower, Upper };
enum class ExpectedAlternative : std::uint8_t { Ok, Err };
enum class ExpectedAccessError : std::uint8_t { NotExpected, InactiveAlternative };
enum class CollectionError : std::uint8_t { NotCollection, TypeMismatch, OutOfMemory };
enum class CollectionInsert : std::uint8_t { Inserted, Updated };
enum class CollectionRemove : std::uint8_t { Removed, Missing };
enum class CollectionProjection : std::uint8_t { Keys, Values };
enum class SetOperation : std::uint8_t { Union, Intersection, Difference };

struct ByteViewRange final {
  std::uint32_t offset;
  std::uint32_t length;
};

class Heap final {
 public:
  [[nodiscard]] StringObject* ConvertStringCase(StringObject& source,
                                                StringCase conversion) noexcept;
  explicit Heap(std::size_t gc_threshold = kDefaultGcThreshold) noexcept;
  ~Heap();

  Heap(Heap&&) = delete;
  Heap& operator=(Heap&&) = delete;
  Heap(const Heap&) = delete;
  Heap& operator=(const Heap&) = delete;

  [[nodiscard]] HeapObject* AllocateInstance(const ClassLayout& layout,
                                             std::span<const Value> initial_fields = {}) noexcept;
  [[nodiscard]] HeapObject* AllocateFuture(Future future) noexcept;
  [[nodiscard]] static Future* GetFuture(const HeapObject& object) noexcept;
  [[nodiscard]] HeapObject* AllocateCompleter(Future future) noexcept;
  [[nodiscard]] static Future* GetCompleter(const HeapObject& object) noexcept;
  [[nodiscard]] HeapObject* AllocateTimer(Timer timer) noexcept;
  [[nodiscard]] static Timer* GetTimer(const HeapObject& object) noexcept;
  [[nodiscard]] HeapObject* AllocateCancellationToken(CancellationToken token) noexcept;
  [[nodiscard]] static CancellationToken* GetCancellationToken(const HeapObject& object) noexcept;
  // VM 的异构全局槽属于 GC 对象，不作为脚本数组或 dynamic 类型暴露。
  [[nodiscard]] HeapObject* AllocateValueSlots(std::size_t count) noexcept;
  [[nodiscard]] std::expected<void, ExecutionStatus> ExtendValueSlots(HeapObject& object,
                                                                      std::size_t count) noexcept;
  [[nodiscard]] static std::span<Value> ValueSlots(HeapObject& object) noexcept;
  void SetValueSlot(HeapObject& object, std::size_t index, Value value) noexcept;
  [[nodiscard]] HeapObject* AllocateArray(const ArrayLayout& layout, std::size_t length) noexcept;
  [[nodiscard]] HeapObject* AllocateByteArray(std::size_t length) noexcept;
  // 调用者保证范围合法；构造过程保根 owner，发布后由视图追踪其强引用。
  [[nodiscard]] HeapObject* AllocateByteData(HeapObject& owner, ByteViewRange range) noexcept;
  [[nodiscard]] static bool IsByteArray(const HeapObject& object) noexcept;
  [[nodiscard]] static bool IsByteData(const HeapObject& object) noexcept;
  [[nodiscard]] static bool IsIntegerArray(const HeapObject& object) noexcept;
  // 借用对象的固定原始存储；调用者先验证对象种类并在借用期间保根。
  [[nodiscard]] static std::span<std::uint8_t> ByteArrayBytes(HeapObject& object) noexcept;
  [[nodiscard]] static std::span<std::uint8_t> ByteDataBytes(HeapObject& object) noexcept;
  [[nodiscard]] HeapObject* AllocateExpected(ClassLayout& layout,
                                             Value payload,
                                             ExpectedAlternative alternative) noexcept;
  [[nodiscard]] static std::expected<ExpectedAlternative, ExpectedAccessError>
  ExpectedAlternativeOf(const HeapObject& object) noexcept;
  [[nodiscard]] static std::expected<Value, ExpectedAccessError> ReadExpectedValue(
      const HeapObject& object,
      ExpectedAlternative alternative) noexcept;
  [[nodiscard]] HeapObject* AllocateCollection(const CollectionLayout& layout) noexcept;
  // 输入在整个运算期间须保根；结果独立分配并保持插入顺序。
  [[nodiscard]] std::expected<HeapObject*, CollectionError>
  CombineSets(const HeapObject& left, const HeapObject& right, SetOperation operation) noexcept;
  // 扩容可能触发 GC；原生调用者须在调用期间保根 object、key 和 value。
  [[nodiscard]] std::expected<CollectionInsert, CollectionError>
  InsertCollection(HeapObject& object, Value key, Value value = Value::Null()) noexcept;
  [[nodiscard]] static std::expected<bool, CollectionError> HasCollectionKey(
      const HeapObject& object,
      Value key) noexcept;
  [[nodiscard]] static std::expected<Value, CollectionError> GetCollectionValue(
      const HeapObject& object,
      Value key) noexcept;
  [[nodiscard]] static std::expected<CollectionRemove, CollectionError> RemoveCollection(
      HeapObject& object,
      Value key) noexcept;
  [[nodiscard]] static std::expected<void, CollectionError> ClearCollection(
      HeapObject& object) noexcept;
  [[nodiscard]] static std::expected<std::uint32_t, CollectionError> CollectionLength(
      const HeapObject& object) noexcept;
  [[nodiscard]] std::expected<void, CollectionError> CopyCollection(
      const HeapObject& object,
      HeapObject& array,
      CollectionProjection projection) noexcept;
  // 输入必须是合法 UTF-8；堆字符串须由调用者保根，分配可触发 GC。
  [[nodiscard]] StringObject* AllocateString(std::string_view first,
                                             std::string_view second = {}) noexcept;
  // parts 不含空指针；所有输入及 separator 的堆存储须保根，最终结果只分配一次。
  [[nodiscard]] StringObject* JoinStrings(std::span<StringObject* const> parts,
                                          std::string_view separator = {}) noexcept;
  // 输入须保根；只格式化 string/int/double/bool/null，不分配中间 GC 字符串。
  [[nodiscard]] std::expected<StringObject*, StringJoinError> JoinValues(
      std::span<const Value> parts) noexcept;
  [[nodiscard]] std::expected<StringObject*, StringJoinError> JoinValues(
      RegisterView parts) noexcept;
  [[nodiscard]] std::expected<StringObject*, StringJoinError> JoinArray(
      const HeapObject& array,
      std::string_view separator) noexcept;
  // 返回视图借用连续存储；数组须可达，且视图不得跨越该数组的扩容或结构变更。
  [[nodiscard]] static std::expected<std::span<StringObject* const>, ArrayAccessError>
  StringElements(const HeapObject& array) noexcept;
  [[nodiscard]] static std::expected<std::span<std::int64_t>, ArrayAccessError> IntegerElements(
      HeapObject& array) noexcept;
  [[nodiscard]] static std::expected<std::uint32_t, ArrayAccessError> ArrayLength(
      const HeapObject& array) noexcept;
  [[nodiscard]] static std::expected<std::uint32_t, ArrayAccessError> ArrayCapacity(
      const HeapObject& array) noexcept;
  [[nodiscard]] static std::expected<std::uint64_t, ArrayAccessError> ArrayMutation(
      const HeapObject& array) noexcept;
  [[nodiscard]] std::expected<void, ArrayAccessError> ReserveArray(HeapObject& array,
                                                                   std::size_t capacity) noexcept;
  [[nodiscard]] std::expected<std::uint32_t, ArrayAccessError> PushArray(HeapObject& array,
                                                                         Value value) noexcept;
  [[nodiscard]] static std::expected<Value, ArrayAccessError> PopArray(HeapObject& array) noexcept;
  // items 是独立的同类型数组；先完成全部分配，再一次性移动后缀并发布新长度。
  [[nodiscard]] std::expected<void, ArrayAccessError> ReplaceArrayRange(HeapObject& array,
                                                                        std::uint32_t start,
                                                                        std::uint32_t count,
                                                                        HeapObject& items) noexcept;
  [[nodiscard]] static std::expected<Value, ArrayAccessError> RemoveArrayElement(
      HeapObject& array,
      std::uint32_t index) noexcept;
  [[nodiscard]] static std::expected<void, ArrayAccessError> ClearArray(HeapObject& array) noexcept;
  [[nodiscard]] std::expected<HeapObject*, ArrayAccessError> SliceArray(HeapObject& array,
                                                                        std::uint32_t start,
                                                                        std::uint32_t end) noexcept;
  [[nodiscard]] std::expected<void, ArrayAccessError> FillArray(HeapObject& array,
                                                                Value value,
                                                                std::uint32_t start,
                                                                std::uint32_t end) noexcept;
  [[nodiscard]] static std::expected<void, ArrayAccessError> ReverseArray(
      HeapObject& array) noexcept;
  [[nodiscard]] HeapObject* AllocateMutableString() noexcept;
  [[nodiscard]] HeapObject* AllocateException(HeapObject& payload,
                                              ExecutionStatus status,
                                              std::span<const TracePosition> trace,
                                              std::string_view message) noexcept;
  // builder 与 text 的堆存储须在扩容期间保根；text 必须是合法 UTF-8。
  [[nodiscard]] std::expected<void, MutableStringError> AppendString(
      HeapObject& builder,
      std::string_view text) noexcept;
  // builder 必须来自本 Heap 的 AllocateMutableString，且调用期间保根。
  [[nodiscard]] StringObject* BuildString(const HeapObject& builder) noexcept;
  [[nodiscard]] static bool IsMutableString(const HeapObject& object) noexcept;

  [[nodiscard]] static Value* GetField(HeapObject& object, std::uint32_t index) noexcept {
    if (object.layout == nullptr || index >= object.layout->field_count()) {
      return nullptr;
    }
    return std::span(ObjectFields(&object), object.layout->field_count()).subspan(index).data();
  }
  [[nodiscard]] static const ClassLayout* ClassOf(const HeapObject& object) noexcept;
  [[nodiscard]] static bool IsValueOfType(Value value, const TypeConstraint& type) noexcept;
  [[nodiscard]] static const Value* GetField(const HeapObject& object,
                                             std::uint32_t index) noexcept {
    if (object.layout == nullptr || index >= object.layout->field_count()) {
      return nullptr;
    }
    return std::span(ObjectFields(&object), object.layout->field_count()).subspan(index).data();
  }
  [[nodiscard]] bool SetField(HeapObject& object, std::uint32_t index, Value value) noexcept {
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
  [[nodiscard]] static std::expected<Value, ArrayAccessError> GetElement(
      const HeapObject& array,
      std::uint32_t index) noexcept;
  [[nodiscard]] std::expected<void, ArrayAccessError> SetElement(HeapObject& array,
                                                                 std::uint32_t index,
                                                                 Value value) noexcept;

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
  void MarkByteData(HeapObject& object) noexcept;
  friend class RootRange;
  friend class Future;

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

  [[nodiscard]] Allocation AllocateRaw(std::size_t requested_size) noexcept;
  void Publish(HeapObject& object, std::uint32_t allocation_size) noexcept;
  void ReleaseRaw(HeapObject& object) noexcept;
  [[nodiscard]] static std::pair<std::size_t, std::size_t> SelectBlockClass(
      std::size_t requested_size) noexcept;
  [[nodiscard]] bool ShouldCollect(std::size_t next_size) const noexcept;
  void MaybeCollect(std::size_t next_size) noexcept;
  void FinishSweep() noexcept;
  void DestroyAll() noexcept;
  [[nodiscard]] StringObject* AllocateStringStorage(std::size_t length) noexcept;
  void ReleaseMutableString(HeapObject& object) noexcept;
  void MarkArray(HeapObject& array) noexcept;
  void ReleaseArray(HeapObject& array) noexcept;
  std::expected<void, ArrayAccessError> GrowArray(HeapObject& array, Value value) noexcept;
  void MarkCollection(HeapObject& object) noexcept;
  void ReleaseCollection(HeapObject& object) noexcept;
  std::expected<void, CollectionError> GrowCollection(HeapObject& object) noexcept;

  HeapObject* objects_ = nullptr;
  HeapObject* gray_ = nullptr;
  HeapObject** sweep_link_ = nullptr;
  RootRange* roots_ = nullptr;
  std::vector<Future::Root> future_roots_;
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
