#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "base/callable.h"
#include "base/declaration.h"
#include "runtime/value/value.h"

namespace vela::runtime {

class Isolate;
class Future;
struct TypeSignature;
struct HeapObject;
struct InstanceStorage;
// Isolate 持有的名义接口身份；不占用可分配类的布局索引。
struct InterfaceType final {
  DeclarationId declaration;
};
// 同一增量会话共享代码身份；函数编号只在该身份内有意义。
struct CodeIdentity final {};
class ClassLayout;

enum class NominalKind : std::uint8_t { Class, Enum, Callable };
struct NominalTypeOptions final {
  NominalKind kind = NominalKind::Class;
  std::vector<const InterfaceType*> interfaces;
  DeclarationId declaration;
};

// 名义身份独立于布局版本。只有固定存储类型保留可用于分配的 fixed_layout；
// 可重载用户类型的字段与方法布局由发布快照和实际实例分别拥有。
class NominalType final {
 public:
  NominalType(const NominalType&) = delete;
  NominalType& operator=(const NominalType&) = delete;
  NominalType(NominalType&&) = delete;
  NominalType& operator=(NominalType&&) = delete;
  ~NominalType() = default;
  [[nodiscard]] bool IsEnum() const noexcept { return kind_ == NominalKind::Enum; }
  [[nodiscard]] bool IsCallable() const noexcept { return kind_ == NominalKind::Callable; }
  [[nodiscard]] bool IsImplementing(const InterfaceType& interface) const noexcept;
  [[nodiscard]] std::span<const InterfaceType* const> interfaces() const noexcept {
    return interfaces_;
  }
  [[nodiscard]] const ClassLayout* fixed_layout() const noexcept { return fixed_layout_; }
  [[nodiscard]] DeclarationId declaration() const noexcept { return declaration_; }

 private:
  friend class Isolate;
  friend class Heap;
  friend class LayoutTransaction;
  explicit NominalType(NominalTypeOptions options);
  NominalKind kind_;
  std::vector<const InterfaceType*> interfaces_;
  ClassLayout* fixed_layout_ = nullptr;
  DeclarationId declaration_;
  // Heap 管理的弱实例链；稳定身份覆盖实例寿命，登记无需分配哈希节点。
  InstanceStorage* instances_ = nullptr;
};

enum class ClassStorage : std::uint8_t { Inline, Reloadable, Host };

struct ClassLayoutOptions final {
  std::optional<CallableTarget> callable;
  std::vector<const InterfaceType*> interfaces;
  std::vector<std::uint32_t> dispatch_targets;
  bool is_enum = false;
  const TypeSignature* callable_type = nullptr;
  NominalType* nominal = nullptr;
  ClassStorage storage = ClassStorage::Inline;
  std::vector<DeclarationId> field_declarations;
};

class ClassLayout final {
 public:
  ClassLayout(const ClassLayout&) = delete;
  ClassLayout& operator=(const ClassLayout&) = delete;
  ClassLayout(ClassLayout&&) = delete;
  ClassLayout& operator=(ClassLayout&&) = delete;
  ~ClassLayout() = default;

  [[nodiscard]] std::uint32_t field_count() const noexcept { return field_count_; }
  [[nodiscard]] std::uint64_t id() const noexcept { return id_; }
  [[nodiscard]] const CallableTarget* callable() const noexcept {
    return callable_ ? &*callable_ : nullptr;
  }
  [[nodiscard]] const NominalType& nominal_type() const noexcept { return *nominal_; }
  [[nodiscard]] NominalType& nominal_type() noexcept { return *nominal_; }
  [[nodiscard]] ClassStorage storage() const noexcept { return storage_; }
  [[nodiscard]] std::optional<std::uint32_t> FieldIndex(DeclarationId field) const noexcept;
  [[nodiscard]] std::span<const DeclarationId> field_declarations() const noexcept {
    return field_declarations_;
  }
  [[nodiscard]] std::span<const InterfaceType* const> interfaces() const noexcept {
    return nominal_->interfaces();
  }
  [[nodiscard]] std::span<const std::uint32_t> dispatch_targets() const noexcept {
    return dispatch_targets_;
  }
  [[nodiscard]] bool IsImplementing(const InterfaceType& interface) const noexcept;
  [[nodiscard]] bool IsEnum() const noexcept { return nominal_->IsEnum(); }
  [[nodiscard]] const TypeSignature* callable_type() const noexcept { return callable_type_; }
  void BindCallableType(const TypeSignature& type) noexcept;
  [[nodiscard]] const CodeIdentity* code_identity() const noexcept { return code_identity_; }
  void BindCodeIdentity(const CodeIdentity& identity) noexcept;
  [[nodiscard]] std::span<const std::uint32_t> reference_fields() const noexcept {
    return reference_fields_;
  }

 private:
  friend class Isolate;
  friend class LayoutTransaction;
  friend class Heap;
  friend struct NativeMemoryLayout;

  ClassLayout(std::uint32_t field_count, std::vector<std::uint32_t> reference_fields);

  std::uint32_t field_count_;
  std::uint64_t id_ = 0;
  std::vector<std::uint32_t> reference_fields_;
  std::optional<CallableTarget> callable_;
  NominalType* nominal_ = nullptr;
  ClassStorage storage_ = ClassStorage::Inline;
  std::vector<DeclarationId> field_declarations_;
  absl::flat_hash_map<DeclarationId, std::uint32_t> field_indices_;
  std::vector<std::uint32_t> dispatch_targets_;
  const TypeSignature* callable_type_ = nullptr;
  const CodeIdentity* code_identity_ = nullptr;
  // 版本布局由 GC 程序状态拥有；实例通过此边追踪其扫描描述的生命周期。
  HeapObject* program_owner_ = nullptr;
};

enum class ObjectKind : std::uint8_t {
  Instance,
  Host,
  Array,
  String,
  MutableString,
  Collection,
  Exception,
  Future,
  Completer,
  Timer,
  CancellationToken,
  ByteArray,
  ByteData,
  RegExp,
  RegExpMatch,
  ValueSlots,
  NativeProgram,
};

struct CollectionLayout;
struct ArrayLayout;

[[nodiscard]] constexpr bool IsLeafObject(ObjectKind kind) noexcept {
  return kind == ObjectKind::String || kind == ObjectKind::MutableString ||
         kind == ObjectKind::ByteArray || kind == ObjectKind::Timer ||
         kind == ObjectKind::CancellationToken || kind == ObjectKind::Host;
}

[[nodiscard]] constexpr bool HasFuturePayload(ObjectKind kind) noexcept {
  return kind == ObjectKind::Future || kind == ObjectKind::Completer;
}

// 尾部存储与头部在同一次分配中；调用者负责容量、对象生命周期及动态长度。
template <class T, class Header>
[[nodiscard]] auto* StorageAfter(Header& header) noexcept {
  using Element = std::conditional_t<std::is_const_v<Header>, const T, T>;
  using Bytes = std::conditional_t<std::is_const_v<Header>, const void, void>;
  static_assert(sizeof(Header) % alignof(T) == 0);
  Bytes* storage = std::span(&header, 1).last(0).data();
  return static_cast<Element*>(storage);
}

struct TypeConstraint final {
  ValueKind kind = ValueKind::Null;
  ObjectKind object_kind = ObjectKind::Instance;
  const NominalType* nominal = nullptr;
  const ArrayLayout* array = nullptr;
  const CollectionLayout* collection = nullptr;
  const InterfaceType* interface = nullptr;
  bool nullable = false;
  bool callable = false;
  bool dynamic = false;
  const TypeSignature* signature = nullptr;

  [[nodiscard]] bool IsSubtypeOf(const TypeConstraint& target) const noexcept;

  bool operator==(const TypeConstraint&) const = default;
  template <typename H>
  friend H AbslHashValue(H hash, const TypeConstraint& type) {
    return H::combine(std::move(hash), type.kind, type.object_kind, type.nominal, type.array,
                      type.collection, type.interface, type.nullable, type.callable, type.dynamic,
                      type.signature);
  }
};

// 结构中的名义类型借用同一个 Isolate 的稳定身份，不借用某一版本的字段布局。
struct TypeSignature final {
  struct Parameter final {
    TypeConstraint type;
    bool optional = false;
    bool rest = false;
    bool operator==(const Parameter&) const = default;
    template <typename H>
    friend H AbslHashValue(H hash, const Parameter& parameter) {
      return H::combine(std::move(hash), parameter.type, parameter.optional, parameter.rest);
    }
  };
  std::vector<Parameter> parameters;
  std::optional<TypeConstraint> result = std::nullopt;
  [[nodiscard]] std::optional<TypeConstraint> FlattenedResult() const noexcept;
  bool operator==(const TypeSignature&) const = default;
  template <typename H>
  friend H AbslHashValue(H hash, const TypeSignature& signature) {
    return H::combine(std::move(hash), signature.parameters, signature.result);
  }
};

// layout 由 Isolate 持有，生命周期覆盖所有数组与编译结果。
struct ArrayLayout final {
  TypeConstraint element{.kind = ValueKind::Integer};

  [[nodiscard]] bool HasTaggedElements() const noexcept {
    if (element.kind == ValueKind::Object || element.kind == ValueKind::String) {
      return element.dynamic;
    }
    return element.nullable || element.dynamic;
  }

  [[nodiscard]] std::size_t element_size() const noexcept;
  [[nodiscard]] bool HasReferences() const noexcept {
    return element.dynamic || element.kind == ValueKind::Object ||
           element.kind == ValueKind::String;
  }
  [[nodiscard]] const TypeConstraint& element_type() const noexcept { return element; }
  [[nodiscard]] TypeConstraint value_type() const noexcept {
    auto type = element;
    if (type.nominal != nullptr && type.nominal->IsEnum()) {
      type.kind = ValueKind::Object;
    }
    return type;
  }
  bool operator==(const ArrayLayout&) const = default;
  template <typename H>
  friend H AbslHashValue(H hash, const ArrayLayout& layout) {
    return H::combine(std::move(hash), layout.element);
  }
};

struct CollectionLayout final {
  ArrayLayout keys;
  std::optional<ArrayLayout> values = std::nullopt;
  bool operator==(const CollectionLayout&) const = default;
  template <typename H>
  friend H AbslHashValue(H hash, const CollectionLayout& layout) {
    return H::combine(std::move(hash), layout.keys, layout.values);
  }
};

struct HeapObject final {
 private:
  friend class Heap;
  friend class StringObject;
  friend struct NativeMemoryLayout;
  friend class InstanceMigration;
  friend void* HostObjectPointer(const HeapObject& object) noexcept;
  friend std::span<std::byte> ObjectFieldStorage(HeapObject& object) noexcept;
  friend std::span<const std::byte> ObjectFieldStorage(const HeapObject& object) noexcept;

  ObjectKind kind = ObjectKind::Instance;
  std::uint8_t mark_bit = 0;
  std::uint8_t size_class = 0;
  std::uint32_t allocation_size = 0;
  const ClassLayout* layout = nullptr;
  HeapObject* next = nullptr;
  HeapObject* gray_next = nullptr;
  struct StringPosition final {
    std::uint32_t index;
    std::uint32_t offset;
  };
  // kind 区分互斥的元数据；与 Value 一样以对象表示保存，避免 inactive union 读取。
  // 字符串仅在此保存整数位置，GC 不将其视为引用。
  alignas(const void*) mutable std::array<std::byte, sizeof(const void*)> type_metadata = {};
  void SetTypeMetadata(const void* metadata) noexcept {
    type_metadata = std::bit_cast<decltype(type_metadata)>(metadata);
  }
  [[nodiscard]] const ArrayLayout* array_layout() const noexcept {
    return static_cast<const ArrayLayout*>(std::bit_cast<const void*>(type_metadata));
  }
  [[nodiscard]] const CollectionLayout* collection_layout() const noexcept {
    return static_cast<const CollectionLayout*>(std::bit_cast<const void*>(type_metadata));
  }
  [[nodiscard]] const TypeSignature* future_signature() const noexcept {
    return static_cast<const TypeSignature*>(std::bit_cast<const void*>(type_metadata));
  }
  // length：String 为 UTF-8 字节数，MutableString 为已用字节数，Array/ValueSlots
  // 为元素数，ByteArray/ByteData 为可见字节数；native callable 的 Instance 为函数编号。
  // Collection 的大小存于尾部 CollectionStorage，其他 kind 不使用本槽。
  std::uint32_t length = 0;
  // 字符串逻辑长度或内联实例的物理字段数；两种对象不共享此槽的含义。
  std::uint32_t auxiliary_length = 0;
  // native_payload：reloadable Instance 持有 InstanceStorage；native callable 借用
  // NativeProgram 对象；Timer、CancellationToken 分别拥有对应句柄；
  // ValueSlots 拥有 Value 数组；NativeProgram 拥有 NativeProgramState。其他 kind 不使用。
  // Future/Completer 的内部句柄位于对象头之后；实例字段块与 callable 环境互斥。
  // Heap 按 kind 追踪引用并销毁其拥有的存储。
  void* native_payload = nullptr;
};

static_assert(alignof(HeapObject) >= alignof(Value));

// 调用者已验证宿主种类并保根包装；借用期间只需固定位置的一次指针读取。
[[nodiscard]] inline void* HostObjectPointer(const HeapObject& object) noexcept {
  return object.native_payload;
}

// 固定布局字段紧接对象头；可重载实例借用独立字段块，地址不得跨安全点保存。
static_assert(sizeof(HeapObject) % alignof(Value) == 0);
[[nodiscard]] std::span<std::byte> ObjectFieldStorage(HeapObject& object) noexcept;
[[nodiscard]] std::span<const std::byte> ObjectFieldStorage(const HeapObject& object) noexcept;

}  // namespace vela::runtime
