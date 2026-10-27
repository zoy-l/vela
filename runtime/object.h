#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "base/callable.h"
#include "runtime/value.h"

namespace vela::runtime {

class Isolate;
class Future;
class Timer;
class CancellationToken;
struct TypeSignature;
// Isolate 持有的名义接口身份；不占用可分配类的字节码索引。
struct InterfaceType final {};
// 同一增量会话共享代码身份；函数编号只在该身份内有意义。
struct CodeIdentity final {};

struct ClassLayoutOptions final {
  std::optional<CallableTarget> callable;
  std::vector<const InterfaceType*> interfaces;
  std::vector<std::uint32_t> dispatch_targets;
  bool is_enum = false;
  const TypeSignature* callable_type = nullptr;
  const TypeSignature* expected_type = nullptr;
};

class ClassLayout final {
 public:
  ClassLayout(const ClassLayout&) = delete;
  ClassLayout& operator=(const ClassLayout&) = delete;
  ClassLayout(ClassLayout&&) = delete;
  ClassLayout& operator=(ClassLayout&&) = delete;
  ~ClassLayout() = default;

  [[nodiscard]] std::uint32_t field_count() const noexcept { return field_count_; }
  [[nodiscard]] const std::optional<CallableTarget>& callable() const noexcept { return callable_; }
  [[nodiscard]] std::span<const InterfaceType* const> interfaces() const noexcept {
    return interfaces_;
  }
  [[nodiscard]] std::span<const std::uint32_t> dispatch_targets() const noexcept {
    return dispatch_targets_;
  }
  [[nodiscard]] bool IsImplementing(const InterfaceType& interface) const noexcept;
  [[nodiscard]] bool IsEnum() const noexcept { return is_enum_; }
  [[nodiscard]] const TypeSignature* callable_type() const noexcept { return callable_type_; }
  void BindCallableType(const TypeSignature& type) noexcept;
  [[nodiscard]] const TypeSignature* expected_type() const noexcept { return expected_type_; }
  [[nodiscard]] const CodeIdentity* code_identity() const noexcept { return code_identity_; }
  void BindCodeIdentity(const CodeIdentity& identity) noexcept;
  void BindExpectedType(const TypeSignature& type) noexcept;
  [[nodiscard]] std::span<const std::uint32_t> reference_fields() const noexcept {
    return reference_fields_;
  }

 private:
  friend class Isolate;

  ClassLayout(std::uint32_t field_count, std::vector<std::uint32_t> reference_fields);

  std::uint32_t field_count_;
  std::vector<std::uint32_t> reference_fields_;
  std::optional<CallableTarget> callable_;
  std::vector<const InterfaceType*> interfaces_;
  std::vector<std::uint32_t> dispatch_targets_;
  bool is_enum_ = false;
  const TypeSignature* callable_type_ = nullptr;
  const TypeSignature* expected_type_ = nullptr;
  const CodeIdentity* code_identity_ = nullptr;
};

enum class ObjectKind : std::uint8_t {
  Instance,
  Array,
  String,
  MutableString,
  Expected,
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
};

struct CollectionLayout;
struct ArrayLayout;

[[nodiscard]] constexpr bool IsLeafObject(ObjectKind kind) noexcept {
  return kind == ObjectKind::String || kind == ObjectKind::MutableString ||
         kind == ObjectKind::ByteArray;
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
  const ClassLayout* instance = nullptr;
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
    return H::combine(std::move(hash), type.kind, type.object_kind, type.instance, type.array,
                      type.collection, type.interface, type.nullable, type.callable, type.dynamic,
                      type.signature);
  }
};

// 结构中的名义类型借用同一个 Isolate 的类布局；规范指针同时用于函数和 Future 载荷身份。
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
    if (type.instance != nullptr && type.instance->IsEnum()) {
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

  ObjectKind kind = ObjectKind::Instance;
  std::uint8_t mark_bit = 0;
  std::uint8_t size_class = 0;
  std::uint32_t allocation_size = 0;
  const ClassLayout* layout = nullptr;
  HeapObject* next = nullptr;
  HeapObject* gray_next = nullptr;
  // kind 区分互斥的元数据；Future/Completer 直接借用载荷签名，不占用数组布局。
  const void* type_metadata = nullptr;
  [[nodiscard]] const ArrayLayout* array_layout() const noexcept {
    return static_cast<const ArrayLayout*>(type_metadata);
  }
  [[nodiscard]] const CollectionLayout* collection_layout() const noexcept {
    return static_cast<const CollectionLayout*>(type_metadata);
  }
  [[nodiscard]] const TypeSignature* future_signature() const noexcept {
    return static_cast<const TypeSignature*>(type_metadata);
  }
  std::uint32_t length = 0;
  // kind 确定 native_payload 的类型；底层存储由 Heap 按该类型销毁。
  void* native_payload = nullptr;
};

static_assert(alignof(HeapObject) >= alignof(Value));

// 实例字段紧接对象头，二者在同一次分配中保持连续存储。
static_assert(sizeof(HeapObject) % alignof(Value) == 0);
[[nodiscard]] inline Value* ObjectFields(HeapObject* object) noexcept {
  return StorageAfter<Value>(*object);
}
[[nodiscard]] inline const Value* ObjectFields(const HeapObject* object) noexcept {
  return StorageAfter<Value>(*object);
}

}  // namespace vela::runtime
