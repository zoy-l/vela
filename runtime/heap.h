#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "base/declaration.h"
#include "base/execution_status.h"
#include "runtime/exception.h"
#include "runtime/external_resources.h"
#include "runtime/future.h"
#include "runtime/object.h"
#include "runtime/register_storage.h"
#include "runtime/value.h"

namespace vela::runtime {

class Heap;
class Isolate;
class NativeMetadataTracer;
struct RegExpStorage;
struct RegExpMatchStorage;
struct NativeProgramState;
struct InstanceStorage;
class InstanceMigration;
struct NativeRetainedFunction;
struct LayoutCounts;

enum class RootOwner : std::uint8_t { Runtime, Host, RootedValue, NativeInstance, NativeHandle };

class RootRange final {
 public:
  explicit RootRange(RootOwner owner = RootOwner::Host) noexcept : owner_(owner) {}
  RootRange(Heap& heap,
            const Value* values,
            std::size_t count,
            RootOwner owner = RootOwner::Host) noexcept;
  RootRange(Heap& heap,
            std::span<StringObject* const> strings,
            RootOwner owner = RootOwner::Host) noexcept;
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
  RootOwner owner_;
  std::uint64_t identity_ = 0;
};

enum class GcPhase : std::uint8_t {
  Idle,
  Mark,
  Sweep,
};

inline constexpr std::size_t kDefaultGcThreshold = 65536;
inline constexpr std::size_t kMaximumByteArrayLength =
    std::numeric_limits<std::uint32_t>::max() - sizeof(HeapObject);

enum class ArrayAccessError : std::uint8_t {
  NotArray,
  OutOfBounds,
  ElementTypeMismatch,
  OutOfMemory,
};

enum class StringJoinError : std::uint8_t { TypeMismatch, OutOfMemory };

enum class StringCase : std::uint8_t { Lower, Upper };

struct CollectionEntry final {
  Value key;
  Value value;
};

// 借用集合；调用者保根对象。结构变更使迭代失效，Next 先检查 mutation 再访问缓存视图。
class CollectionIterator final {
 public:
  CollectionIterator() noexcept = default;
  [[nodiscard]] std::expected<std::optional<CollectionEntry>, ExecutionStatus> Next() noexcept;

 private:
  friend class Heap;
  const HeapObject* object_ = nullptr;
  std::uint64_t mutation_ = 0;
  std::uint32_t next_ = 0;
  std::span<const std::byte> keys_;
  std::span<const std::byte> values_;
  std::span<const std::uint32_t> links_;
};
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
  Heap(Isolate& isolate, std::size_t gc_threshold) noexcept;
  ~Heap();

  Heap(Heap&&) = delete;
  Heap& operator=(Heap&&) = delete;
  Heap(const Heap&) = delete;
  Heap& operator=(const Heap&) = delete;

  [[nodiscard]] HeapObject* AllocateInstance(const ClassLayout& layout,
                                             std::span<const Value> initial_fields = {}) noexcept;
  // 返回弱登记中的对象；调用者在可能分配前统一保根，并按需完成一次新根追踪。
  void AppendInstances(const NominalType& type, std::vector<Value>& instances) const;
  // 调用者先完成当前根的一致追踪；仅返回实际可达闭包及 C++ 版本根的函数集合。
  void AppendRetainedNativeFunctions(std::vector<NativeRetainedFunction>& functions) const;
  void AppendRetainedNativeTypes(std::vector<DeclarationId>& declarations) const;
  void AccumulateNativeLayouts(LayoutCounts& counts) const noexcept;
  [[nodiscard]] std::unique_ptr<InstanceMigration> PrepareInstanceMigration(
      HeapObject& object,
      const ClassLayout& layout) noexcept;
  [[nodiscard]] HeapObject* AllocateNativeProgram(
      std::unique_ptr<NativeProgramState> state) noexcept;
  [[nodiscard]] static NativeProgramState& NativeProgram(HeapObject& object) noexcept;
  void SetNativeProgramSlot(HeapObject& object, std::size_t index, Value value) noexcept;
  void SetNativeEnvironment(HeapObject& callable,
                            HeapObject& state,
                            std::uint32_t function) noexcept;
  [[nodiscard]] static HeapObject* NativeEnvironment(const HeapObject& callable) noexcept;
  [[nodiscard]] static std::uint32_t NativeFunctionIndex(const HeapObject& callable) noexcept;
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
  // 原生块参与堆压力核算；调用者负责配对释放，分配期间须保根所有借用对象。
  [[nodiscard]] void* AllocateNative(std::size_t size) noexcept;
  void FreeNative(void* pointer) noexcept;
  [[nodiscard]] HeapObject* AllocateRegExp(const RegExpStorage& storage) noexcept;
  // 引用在发布前初始化；调用者随后填充不含引用的 captures，完成前不得暴露给消费者。
  [[nodiscard]] HeapObject* AllocateRegExpMatch(const RegExpMatchStorage& storage) noexcept;
  [[nodiscard]] static RegExpStorage& RegExpOf(HeapObject& object) noexcept;
  [[nodiscard]] static RegExpMatchStorage& RegExpMatchOf(HeapObject& object) noexcept;
  [[nodiscard]] static bool IsRegExp(const HeapObject& object) noexcept;
  [[nodiscard]] static bool IsRegExpMatch(const HeapObject& object) noexcept;
  // 调用者保证范围合法；构造过程保根 owner，发布后由视图追踪其强引用。
  [[nodiscard]] HeapObject* AllocateByteData(HeapObject& owner, ByteViewRange range) noexcept;
  [[nodiscard]] static bool IsByteArray(const HeapObject& object) noexcept;
  [[nodiscard]] static bool IsByteData(const HeapObject& object) noexcept;
  // 借用对象的固定原始存储；调用者先验证对象种类并在借用期间保根。
  [[nodiscard]] static std::span<std::uint8_t> ByteArrayBytes(HeapObject& object) noexcept;
  [[nodiscard]] static std::span<std::uint8_t> ByteDataBytes(HeapObject& object) noexcept;
  [[nodiscard]] HeapObject* AllocateCollection(const CollectionLayout& layout) noexcept;
  // 输入在整个运算期间须保根；结果独立分配并保持插入顺序。
  [[nodiscard]] std::expected<HeapObject*, ExecutionStatus>
  CombineSets(const HeapObject& left, const HeapObject& right, SetOperation operation) noexcept;
  // 扩容可能触发 GC；原生调用者须在调用期间保根 object、key 和 value。
  [[nodiscard]] std::expected<CollectionInsert, ExecutionStatus>
  InsertCollection(HeapObject& object, Value key, Value value = Value::Null()) noexcept;
  [[nodiscard]] static std::expected<bool, ExecutionStatus> HasCollectionKey(
      const HeapObject& object,
      Value key) noexcept;
  [[nodiscard]] static std::expected<Value, ExecutionStatus> GetCollectionValue(
      const HeapObject& object,
      Value key) noexcept;
  [[nodiscard]] static std::expected<CollectionRemove, ExecutionStatus> RemoveCollection(
      HeapObject& object,
      Value key) noexcept;
  [[nodiscard]] static std::expected<void, ExecutionStatus> ClearCollection(
      HeapObject& object) noexcept;
  [[nodiscard]] static std::expected<std::uint32_t, ExecutionStatus> CollectionLength(
      const HeapObject& object) noexcept;
  [[nodiscard]] static std::expected<CollectionIterator, ExecutionStatus> IterateCollection(
      const HeapObject& object) noexcept;
  [[nodiscard]] std::expected<void, ExecutionStatus> CopyCollection(
      const HeapObject& object,
      HeapObject& array,
      CollectionProjection projection) noexcept;
  // 输入必须是合法 UTF-8；堆字符串须由调用者保根，分配可触发 GC。
  [[nodiscard]] StringObject* AllocateString(std::string_view first,
                                             std::string_view second = {}) noexcept;
  // text 须为合法 UTF-8；输入及目标须保根，string 数组长度须等于 grapheme 数量。
  [[nodiscard]] std::expected<void, ExecutionStatus> FillGraphemes(HeapObject& array,
                                                                   std::string_view text);
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
  // builder 来自本 Heap 的 AllocateMutableString；builder 与 text 须保根，text 为合法 UTF-8。
  [[nodiscard]] std::expected<void, ExecutionStatus> AppendString(HeapObject& builder,
                                                                  std::string_view text) noexcept;
  // 追加未初始化字节，调用者须立即填充；视图不得跨越 builder 的下一次追加或扩容。
  [[nodiscard]] std::expected<std::span<char>, ExecutionStatus> AppendStringStorage(
      HeapObject& builder,
      std::size_t count) noexcept;
  // builder 必须来自本 Heap 的 AllocateMutableString，且调用期间保根。
  [[nodiscard]] StringObject* BuildString(const HeapObject& builder) noexcept;
  [[nodiscard]] static bool IsMutableString(const HeapObject& object) noexcept;

  template <class Object>
    requires std::is_same_v<std::remove_const_t<Object>, HeapObject>
  [[nodiscard]] static std::conditional_t<std::is_const_v<Object>, const Value*, Value*> GetField(
      Object& object,
      std::uint32_t index) noexcept {
    const auto* layout = object.layout;
    if (layout == nullptr || index >= layout->field_count()) {
      return nullptr;
    }
    return std::span(ObjectFields(&object), layout->field_count()).subspan(index).data();
  }
  [[nodiscard]] static const ClassLayout* ClassOf(const HeapObject& object) noexcept;
  [[nodiscard]] static ObjectKind KindOf(const HeapObject& object) noexcept { return object.kind; }
  [[nodiscard]] static TypeConstraint TypeOf(Value value) noexcept;
  [[nodiscard]] static const ArrayLayout* ArrayTypeOf(const HeapObject& object) noexcept {
    return object.kind == ObjectKind::Array ? object.array_layout() : nullptr;
  }
  [[nodiscard]] static const CollectionLayout* CollectionTypeOf(const HeapObject& object) noexcept {
    return object.kind == ObjectKind::Collection ? object.collection_layout() : nullptr;
  }
  [[nodiscard]] static bool IsValueOfType(Value value, const TypeConstraint& type) noexcept;
  [[nodiscard]] static bool IsThrowable(Value value) noexcept;
  // Future/Completer 的类型身份由生产者绑定；签名由 Isolate 持有，禁止重标已有身份。
  [[nodiscard]] static std::expected<void, ExecutionStatus> BindRuntimeType(
      HeapObject& object,
      const TypeConstraint& type) noexcept;
  [[nodiscard]] bool SetField(HeapObject& object, std::uint32_t index, Value value) noexcept;
  // 原生代码已完成类型化存储；引用写入仍须经过同一个增量 GC 屏障。
  void WriteBarrier(HeapObject& source, Value value) noexcept;
  // receiver 为本 Heap 的数组，value 已按元素类型验证；两者在可能扩容的 GC 中保根。
  [[nodiscard]] std::expected<void, ArrayAccessError> GrowArray(HeapObject& array,
                                                                Value value) noexcept;
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

  [[nodiscard]] GcPhase phase() const noexcept { return phase_; }
  [[nodiscard]] bool IsCollecting() const noexcept { return phase_ != GcPhase::Idle; }
  [[nodiscard]] std::size_t object_count() const noexcept { return object_count_; }
  [[nodiscard]] std::size_t allocated_bytes() const noexcept { return allocated_bytes_; }
  [[nodiscard]] std::size_t committed_bytes() const noexcept { return committed_bytes_; }
  // 外部字段块载荷，包含已准备的迁移块，不含分配器头部。
  [[nodiscard]] std::size_t instance_field_bytes() const noexcept { return instance_field_bytes_; }
  [[nodiscard]] std::size_t instance_type_count() const noexcept { return instances_.size(); }
  // 限制 Heap 核算的托管及外部块活跃字节；降低限额不改变已分配状态，后续申请显式失败。
  void SetAllocationLimit(std::size_t bytes) noexcept { allocation_limit_ = bytes; }
  [[nodiscard]] std::size_t allocation_limit() const noexcept { return allocation_limit_; }
  void AppendExternalRoots(std::vector<ExternalResourceInfo>& result) const;

 private:
  friend class Isolate;
  void TraceNativeMetadata(NativeMetadataTracer& tracer) const;
  Isolate* isolate_ = nullptr;
  void MarkByteData(HeapObject& object) noexcept;
  void MarkNativeProgram(HeapObject& object) noexcept;
  void ReleaseNativeProgram(HeapObject& object) noexcept;
  void ReleaseRetiredNativePrograms() noexcept;
  void MarkRegExp(HeapObject& object) noexcept;
  static void ReleaseRegExp(HeapObject& object) noexcept;
  friend class RootRange;
  friend class NativeMaterialization;
  friend class Future;
  friend class InstanceMigration;
  friend class NativeDispatch;
  [[nodiscard]] static InstanceStorage& InstanceFields(HeapObject& object) noexcept;
  [[nodiscard]] static std::expected<const ArrayLayout*, ArrayAccessError> ElementLayout(
      const HeapObject& array,
      std::uint32_t index) noexcept;
  [[nodiscard]] static std::expected<const ArrayLayout*, ArrayAccessError> ValueLayout(
      const HeapObject& array,
      Value value) noexcept;

  static constexpr auto kSizeClasses =
      std::to_array<std::size_t>({32, 64, 128, 256, 512, 1024, 2048, 4096});
  static constexpr std::size_t kAutomaticWork = 16;

  void RegisterRoot(RootRange& root) noexcept;
  static void UnregisterRoot(RootRange& root) noexcept;
  void MarkRoots() noexcept;
  void MarkRoot(const RootRange& root) noexcept;
  void MarkValue(Value value) noexcept;
  void MarkReference(Value value) noexcept { MarkValue(value); }
  void MarkReference(HeapObject* object) noexcept;
  void MarkReference(StringObject* string) noexcept;
  void MarkObject(HeapObject& object) noexcept;
  void ProcessGrayObject(HeapObject& object) noexcept;
  void DrainGray() noexcept;
  void WriteBarrier(HeapObject& source,
                    const ArrayLayout& layout,
                    std::span<const std::byte> storage) noexcept;
  void MarkElements(const ArrayLayout& layout, std::span<const std::byte> storage) noexcept;

  struct Allocation final {
    void* memory = nullptr;
    std::uint32_t size = 0;
    std::uint8_t size_class = 0;
  };

  template <class Initialize>
  [[nodiscard]] HeapObject* AllocateObject(ObjectKind kind,
                                           std::size_t size,
                                           const Initialize& initialize) noexcept;
  template <class T>
  [[nodiscard]] HeapObject* AllocateNativeObject(ObjectKind kind, T payload) noexcept;
  template <ObjectKind kind, class T>
  [[nodiscard]] static T* NativeOf(const HeapObject& object) noexcept {
    return object.kind == kind ? static_cast<T*>(object.native_payload) : nullptr;
  }

  [[nodiscard]] Allocation AllocateRaw(std::size_t requested_size) noexcept;
  void Publish(HeapObject& object, const Allocation& allocation) noexcept;
  void ReleaseRaw(HeapObject& object) noexcept;
  void ReleaseExternalStorage(void* storage, std::size_t bytes) noexcept;
  [[nodiscard]] static std::pair<std::size_t, std::size_t> SelectBlockClass(
      std::size_t requested_size) noexcept;
  [[nodiscard]] bool ShouldCollect(std::size_t next_size) const noexcept;
  [[nodiscard]] bool CanAllocate(std::size_t bytes) const noexcept;
  void MaybeCollect(std::size_t next_size) noexcept;
  void FinishSweep() noexcept;
  void DestroyAll() noexcept;
  [[nodiscard]] InstanceStorage* AllocateInstanceStorage(std::uint32_t count) noexcept;
  void ReleaseInstanceStorage(InstanceStorage& storage) noexcept;
  void RegisterInstance(HeapObject& object, InstanceStorage& storage);
  void ReleaseInstance(HeapObject& object) noexcept;
  struct StringSize final {
    std::size_t bytes;
    std::size_t code_points;
  };
  [[nodiscard]] StringObject* AllocateStringStorage(StringSize size) noexcept;
  void ReleaseMutableString(HeapObject& object) noexcept;
  void MarkArray(HeapObject& array) noexcept;
  void ReleaseArray(HeapObject& array) noexcept;
  [[nodiscard]] HeapObject* AllocateArrayStorage(const ArrayLayout& layout,
                                                 std::size_t length,
                                                 std::span<const std::byte> contents) noexcept;
  [[nodiscard]] std::expected<void, ArrayAccessError> ReserveArrayStorage(
      HeapObject& array,
      std::size_t capacity) noexcept;
  [[nodiscard]] static Value RemoveArrayStorage(HeapObject& array, std::uint32_t index) noexcept;
  void MarkCollection(HeapObject& object) noexcept;
  void ReleaseCollection(HeapObject& object) noexcept;
  std::expected<void, ExecutionStatus> GrowCollection(HeapObject& object) noexcept;
  std::expected<CollectionInsert, ExecutionStatus> InsertCollectionHashed(HeapObject& object,
                                                                          Value key,
                                                                          std::size_t hash,
                                                                          Value value) noexcept;

  HeapObject* objects_ = nullptr;
  HeapObject* gray_ = nullptr;
  HeapObject** sweep_link_ = nullptr;
  RootRange* roots_ = nullptr;
  std::uint64_t next_root_identity_ = 0;
  std::vector<Future::Root> future_roots_;
  absl::flat_hash_map<const NominalType*, InstanceStorage*> instances_;
  NativeProgramState* retired_programs_ = nullptr;
  std::array<HeapObject*, kSizeClasses.size()> free_blocks_{};

  std::size_t object_count_ = 0;
  std::size_t allocated_bytes_ = 0;
  std::size_t committed_bytes_ = 0;
  std::size_t instance_field_bytes_ = 0;
  std::size_t allocation_limit_ = std::numeric_limits<std::size_t>::max();
  std::size_t gc_threshold_ = 0;
  std::size_t minimum_gc_threshold_ = 0;
  std::uint8_t current_mark_bit_ = 1;
  GcPhase phase_ = GcPhase::Idle;
};

// 准备中的块和被迁移对象分别保根；销毁候选只释放新块，Commit 不分配也不执行用户代码。
class InstanceMigration final {
 public:
  ~InstanceMigration();
  InstanceMigration(const InstanceMigration&) = delete;
  InstanceMigration& operator=(const InstanceMigration&) = delete;
  InstanceMigration(InstanceMigration&&) = delete;
  InstanceMigration& operator=(InstanceMigration&&) = delete;
  void Commit() noexcept;

 private:
  friend class Heap;
  InstanceMigration(Heap& heap, HeapObject& object, const ClassLayout& layout) noexcept;
  Heap* heap_;
  Value object_;
  const ClassLayout* layout_;
  InstanceStorage* storage_ = nullptr;
  RootRange object_root_;
  RootRange field_roots_;
};

// 初始化不得分配或失败；对象的引用及有效长度全部就绪后才发布给 GC。
template <class Initialize>
HeapObject* Heap::AllocateObject(ObjectKind kind,
                                 std::size_t size,
                                 const Initialize& initialize) noexcept {
  MaybeCollect(size);
  const auto allocation = AllocateRaw(size);
  if (allocation.memory == nullptr) {
    return nullptr;
  }
  auto* object = std::construct_at(static_cast<HeapObject*>(allocation.memory));
  object->kind = kind;
  initialize(*object);
  Publish(*object, allocation);
  return object;
}

template <class T>
HeapObject* Heap::AllocateNativeObject(ObjectKind kind, T payload) noexcept {
  auto owned = std::unique_ptr<T>(new (std::nothrow) T(std::move(payload)));
  if (owned == nullptr) {
    return nullptr;
  }
  return AllocateObject(kind, sizeof(HeapObject), [&owned](HeapObject& object) {
    if constexpr (std::is_same_v<T, Future>) {
      owned->MakeHeapOwned();
    }
    object.native_payload = owned.release();
  });
}

}  // namespace vela::runtime
