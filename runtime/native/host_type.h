#pragma once

#include <cstddef>
#include <expected>

#include "base/execution_status.h"
#include "runtime/heap/heap.h"
#include "runtime/native/native_type.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {

class Isolate;
class NativeDispatch;
class NativeInstance;

// 返回结构元数据的借用；调用者须在下一次分配前建立类型根或长期所有者。
[[nodiscard]] std::expected<TypeConstraint, ExecutionStatus> ResolveInstanceNativeType(
    NativeInstance& instance,
    NativeType description);

// 共享不可变本地契约。封闭程序保留环境；会话只保留稳定类型身份，操作时选择当前发布。
class HostType final {
 public:
  ~HostType();
  HostType(const HostType& other) noexcept;
  HostType& operator=(const HostType& other) noexcept;
  HostType(HostType&& other) noexcept;
  HostType& operator=(HostType&& other) noexcept;

  [[nodiscard]] static std::expected<HostType, ExecutionStatus> Bind(Isolate& isolate,
                                                                     NativeType description);
  [[nodiscard]] static std::expected<HostType, ExecutionStatus> Bind(NativeInstance& instance,
                                                                     NativeType description);
  [[nodiscard]] static std::expected<HostType, ExecutionStatus> Bind(NativeDispatch& dispatch,
                                                                     NativeType description);
  [[nodiscard]] Isolate& isolate() const noexcept;
  [[nodiscard]] TypeConstraint constraint() const noexcept;
  [[nodiscard]] std::expected<HeapObject*, ExecutionStatus> program() const noexcept;

 private:
  struct Storage final {
    Storage(Isolate& owner, TypeConstraint contract, Value state, NativeDispatch* session) noexcept;
    Isolate* isolate;
    TypeConstraint constraint;
    Value anchor;
    NativeDispatch* dispatch;
    RootRange program_root;
    RootRange type_root;
    std::size_t references = 1;
  };
  explicit HostType(Storage& storage) noexcept : storage_(&storage) {}
  [[nodiscard]] static std::expected<HostType, ExecutionStatus> Create(Isolate& isolate,
                                                                       TypeConstraint constraint,
                                                                       Value anchor,
                                                                       NativeDispatch* dispatch);
  void Retain() noexcept;
  void Reset() noexcept;
  Storage* storage_;
};

}  // namespace vela::runtime
