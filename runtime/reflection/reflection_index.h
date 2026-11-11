#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include <absl/functional/function_ref.h>

#include "base/callable.h"
#include "base/declaration.h"
#include "base/execution_status.h"
#include "base/reflection.h"
#include "runtime/heap/host_memory.h"
#include "runtime/heap/native_buffer.h"
#include "runtime/native/native_image.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {

class Heap;
class NativeInstance;

struct ReflectedType final {
  TypeConstraint constraint;
  bool is_void = false;
  [[nodiscard]] bool IsInstance(Value value) const noexcept;
  [[nodiscard]] std::expected<bool, ExecutionStatus> IsAssignableFrom(
      const ReflectedType& other) const noexcept;
  bool operator==(const ReflectedType&) const = default;
  template <class H>
  friend H AbslHashValue(H state, const ReflectedType& type) {
    return H::combine(std::move(state), type.constraint, type.is_void);
  }
};

// 索引只借用所属程序的不可变映像和类型图；存储按 Heap 记账，随程序状态一起释放。
class ReflectionIndex final {
 public:
  [[nodiscard]] static std::expected<std::unique_ptr<ReflectionIndex>, ExecutionStatus>
  Create(Heap& heap, const NativeImage& image, std::span<const TypeConstraint> types);
  ~ReflectionIndex();
  ReflectionIndex(const ReflectionIndex&) = delete;
  ReflectionIndex& operator=(const ReflectionIndex&) = delete;
  ReflectionIndex(ReflectionIndex&&) = delete;
  ReflectionIndex& operator=(ReflectionIndex&&) = delete;

  [[nodiscard]] ReflectedType TypeAt(std::uint32_t index) const noexcept;
  [[nodiscard]] const NativeReflectionType* Type(ReflectedType type) const noexcept;
  [[nodiscard]] const NativeReflectionType* FindType(
      std::string_view module,
      std::string_view name,
      std::span<const ReflectedType> arguments) const;
  [[nodiscard]] const NativeReflectionType* FindDescribedType(
      std::string_view module,
      std::string_view name,
      std::span<const ReflectedType> arguments = {}) const;
  void EnumerateTypes(std::string_view module,
                      absl::FunctionRef<void(const NativeReflectionType&)> visit) const;
  [[nodiscard]] const NativeReflectionMember* RegisteredAccessor(
      const NativeReflectionMember& property,
      reflection::MemberKind kind) const noexcept;
  [[nodiscard]] std::expected<const NativeReflectionMember*, ExecutionStatus> ResolveAccessor(
      const NativeReflectionMember& property,
      reflection::MemberKind kind) const noexcept;
  [[nodiscard]] bool CanAccess(const NativeReflectionMember& property,
                               reflection::MemberKind kind) const noexcept;
  [[nodiscard]] std::expected<const NativeReflectionMember*, ExecutionStatus> ResolveMember(
      DeclarationId declaration) const noexcept;
  [[nodiscard]] std::expected<const CallSignature*, ExecutionStatus> Signature(
      const NativeReflectionMember& member);
  [[nodiscard]] std::expected<const NativeReflectionMember*, ExecutionStatus> FindMember(
      ReflectedType type,
      reflection::MemberKind kind,
      std::string_view name,
      reflection::MemberQuery query,
      std::span<const ReflectedType> arguments = {}) const;
  [[nodiscard]] std::expected<void, ExecutionStatus> EnumerateMembers(
      ReflectedType type,
      reflection::MemberKind kind,
      reflection::MemberQuery query,
      absl::FunctionRef<void(const NativeReflectionMember&)> visit) const;
  void EnumerateMethodAttributes(
      ReflectedType type,
      std::optional<std::string_view> module,
      absl::FunctionRef<void(const NativeAttribute&, const NativeReflectionMember&)> visit) const;

 private:
  struct Lookup final {
    std::uint64_t key = 0;
    std::uint32_t index = 0;
  };
  struct View final {
    std::span<const std::uint32_t> declared;
    std::uint32_t* inherited = nullptr;
    std::size_t size = 0;
    Lookup* lookup = nullptr;
    bool prepared = false;
  };

  ReflectionIndex(Heap& heap, const NativeImage& image, std::span<const TypeConstraint> types);
  [[nodiscard]] bool IsAllocated() const noexcept;
  [[nodiscard]] std::expected<void, ExecutionStatus> Prepare();
  [[nodiscard]] std::expected<const CallSignature*, ExecutionStatus> PrepareSignature(
      const NativeReflectionMember& member,
      CallSignature*& cached);
  [[nodiscard]] const NativeReflectionMember* Member(DeclarationId declaration) const noexcept;
  [[nodiscard]] std::expected<void, ExecutionStatus> PrepareAttributes();
  [[nodiscard]] const NativeReflectionMember* AttributedMethod(
      const NativeAttribute& attribute) const;
  [[nodiscard]] std::expected<void, ExecutionStatus> PrepareView(std::uint32_t index);
  void MergeInheritedMembers(View& view, std::span<const std::uint32_t> inherited);
  [[nodiscard]] std::optional<std::uint32_t> TypePosition(std::uint32_t index) const noexcept;
  [[nodiscard]] std::optional<std::uint32_t> TypePosition(ReflectedType type) const noexcept;
  [[nodiscard]] std::expected<std::optional<std::uint32_t>, ExecutionStatus> QueryType(
      ReflectedType type,
      reflection::MemberKind kind) const;
  [[nodiscard]] std::span<const std::uint32_t> Entries(
      std::uint32_t type,
      reflection::MemberScope scope) const noexcept;
  [[nodiscard]] bool MatchesArguments(std::span<const std::uint32_t> indices,
                                      std::span<const ReflectedType> arguments) const noexcept;
  [[nodiscard]] std::uint64_t MemberKey(const NativeReflectionMember& member) const;

  HostMemory memory_;
  Heap* heap_;
  const NativeImage* image_;
  std::span<const TypeConstraint> types_;
  NativeBuffer<View> views_;
  NativeBuffer<Lookup> type_positions_;
  NativeBuffer<Lookup> type_constraints_;
  NativeBuffer<Lookup> type_names_;
  NativeBuffer<Lookup> member_ids_;
  NativeBuffer<std::uint32_t> declared_;
  NativeBuffer<CallSignature*> signatures_;
  std::size_t signature_bytes_ = sizeof(ReflectionIndex);
  Lookup* attribute_lookup_ = nullptr;
  std::size_t attribute_count_ = 0;
};

[[nodiscard]] std::expected<ReflectionIndex*, ExecutionStatus> PrepareReflectionIndex(
    NativeInstance& instance);

}  // namespace vela::runtime
