#pragma once

#include <cstdint>
#include <optional>

#include "runtime/heap.h"
#include "runtime/value.h"

namespace vela::runtime {

struct NativeProgramState;
struct NativeImage;

enum class NativeDependencyPhase : std::uint8_t { Instruction, AwaitingResult };

struct NativeRetainedFunction final {
  const NativeImage* image = nullptr;
  std::uint32_t function = 0;
  // 空值表示函数值可从入口重入；活跃帧和挂起续延只保留当前位置可达的依赖。
  std::optional<std::uint32_t> instruction = std::nullopt;
  NativeDependencyPhase phase = NativeDependencyPhase::Instruction;
};

// C++ 版本引用保根其程序状态并登记具体函数；程序快照自身不使所有函数成为存留依赖。
// AOT 状态沿用原对象图所有权，避免全局 Future 与挂起激活形成永久根环。
class NativeFunctionRoot final {
 public:
  NativeFunctionRoot(HeapObject& state,
                     std::uint32_t function,
                     RootOwner owner = RootOwner::Runtime,
                     std::optional<std::uint32_t> instruction = std::nullopt) noexcept;
  ~NativeFunctionRoot();
  NativeFunctionRoot(const NativeFunctionRoot&) = delete;
  NativeFunctionRoot& operator=(const NativeFunctionRoot&) = delete;
  NativeFunctionRoot(NativeFunctionRoot&&) = delete;
  NativeFunctionRoot& operator=(NativeFunctionRoot&&) = delete;
  [[nodiscard]] Value state() const noexcept { return state_; }
  [[nodiscard]] std::uint32_t function() const noexcept { return function_; }
  void SetExecuting() noexcept;
  void SetSuspended(std::uint32_t instruction) noexcept;

 private:
  friend class Heap;
  void RegisterDependency() noexcept;
  void UnregisterDependency() noexcept;
  Value state_;
  std::uint32_t function_;
  NativeProgramState* retained_ = nullptr;
  std::optional<std::uint32_t> instruction_;
  NativeFunctionRoot* next_ = nullptr;
  NativeFunctionRoot** previous_link_ = nullptr;
  RootRange root_;
};

}  // namespace vela::runtime
