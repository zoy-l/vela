#pragma once

#include <expected>

#include "base/execution_status.h"
#include "runtime/value/value.h"

namespace vela::runtime {

class Isolate;

// 公共 Persistent、错误载荷和 C 持久句柄共用的不可变根槽；所有操作属于 Isolate 线程。
class HostRoot final {
 public:
  HostRoot() noexcept = default;
  ~HostRoot();
  HostRoot(const HostRoot& other) noexcept;
  HostRoot& operator=(const HostRoot& other) noexcept;
  HostRoot(HostRoot&& other) noexcept;
  HostRoot& operator=(HostRoot&& other) noexcept;

  [[nodiscard]] static std::expected<HostRoot, ExecutionStatus> Create(Isolate& isolate,
                                                                       Value value) noexcept;
  [[nodiscard]] bool IsEmpty() const noexcept { return storage_ == nullptr; }
  [[nodiscard]] Isolate& isolate() const noexcept;
  [[nodiscard]] Value value() const noexcept;
  void Reset() noexcept;

 private:
  struct Storage;
  explicit HostRoot(Storage& storage) noexcept : storage_(&storage) {}
  void Retain() noexcept;
  Storage* storage_ = nullptr;
};

}  // namespace vela::runtime
