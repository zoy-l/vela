#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "base/execution_limits.h"
#include "base/execution_status.h"
#include "runtime/exception.h"
#include "runtime/heap.h"
#include "runtime/value.h"
#include "runtime/value_buffer.h"

namespace vela::runtime {

class Isolate;
class NativeInstance;
struct FutureDiagnostic;
struct FutureResult;

inline constexpr std::uint32_t kNativeAbiVersion = 1;
inline constexpr std::size_t kNativeValueBytes = 16;

enum class NativeValueKind : std::uint8_t {
  Null,
  Boolean,
  Integer,
  Double,
  Object,
  String,
};

// 仅跨宿主包装边界使用；编译单元内部的参数和局部值保持类型化。
struct NativeValue final {
  std::uint64_t payload = 0;
  NativeValueKind kind = NativeValueKind::Null;
  std::uint8_t reserved_byte = 0;
  std::uint16_t reserved_half = 0;
  std::uint32_t reserved_word = 0;
};
static_assert(sizeof(NativeValue) == kNativeValueBytes);

struct NativeFrameSize final {
  std::size_t root_count = 0;
  std::size_t storage_bytes = 0;
};

// 仅借用当前资源帧，直到 frame_release；嵌套调用不改变外层帧的地址。
struct NativeFrameAccess final {
  Value* roots = nullptr;
  void* storage = nullptr;
};

// 逻辑栈由生成代码的原生栈持有；资源帧独立按需构造。
struct NativeTraceFrame final {
  TracePosition position;
  NativeTraceFrame* previous = nullptr;
};

class NativeFrame final {
 public:
  NativeFrame(Isolate& isolate, NativeFrameSize size, Value* stack_roots) noexcept;
  ~NativeFrame();
  NativeFrame(const NativeFrame&) = delete;
  NativeFrame& operator=(const NativeFrame&) = delete;
  NativeFrame(NativeFrame&&) = delete;
  NativeFrame& operator=(NativeFrame&&) = delete;

  [[nodiscard]] std::span<Value> roots() noexcept { return roots_; }
  [[nodiscard]] bool IsAllocated() const noexcept {
    return allocated_roots_.IsAllocated() && (storage_bytes_ == 0 || storage_ != nullptr);
  }
  [[nodiscard]] void* storage() const noexcept { return storage_; }

 private:
  ValueBuffer allocated_roots_;
  std::span<Value> roots_;
  RootRange root_range_;
  std::size_t storage_bytes_;
  void* storage_;
};

// 仅消费由编译器或运行时按 NativeValue 契约构造的值，不验证任意外部位模式。
[[nodiscard]] NativeValue EncodeNativeValue(Value value) noexcept;
[[nodiscard]] Value DecodeNativeValue(const NativeValue& value) noexcept;

// 每次执行独占；不改变宿主 NativeContext 的生命周期和责任。
// Isolate 必须晚于此对象销毁，所有操作发生在 Isolate 所属线程。
class NativeExecution final {
 public:
  explicit NativeExecution(NativeInstance& instance) noexcept;
  ~NativeExecution() = default;
  NativeExecution(const NativeExecution&) = delete;
  NativeExecution& operator=(const NativeExecution&) = delete;
  NativeExecution(NativeExecution&&) = delete;
  NativeExecution& operator=(NativeExecution&&) = delete;

  [[nodiscard]] ExecutionStatus Enter(NativeTraceFrame& frame, std::uint32_t function) noexcept;
  [[nodiscard]] ExecutionStatus PrepareEntry() noexcept;
  // 实际入口已经检查深度；延迟登记不得清理被调用方刚产生的异常。
  void MaterializeFrame(NativeTraceFrame& frame, std::uint32_t function) noexcept;
  void Exit() noexcept;
  [[nodiscard]] std::size_t frame_count() const noexcept { return frame_count_; }
  [[nodiscard]] NativeInstance& instance() const noexcept { return *instance_; }
  [[nodiscard]] Isolate& isolate() const noexcept { return *isolate_; }
  [[nodiscard]] std::uint32_t* current_instruction() noexcept {
    return &current_frame_->position.instruction;
  }
  [[nodiscard]] ExecutionStatus HandleFailure(ExecutionStatus status,
                                              std::string_view message = {});
  [[nodiscard]] FutureDiagnostic Diagnostic(ExecutionStatus status, std::string_view message) const;
  [[nodiscard]] FutureDiagnostic ExceptionDiagnostic() const;
  [[nodiscard]] ExecutionStatus RestoreFutureError(const FutureResult& completion);
  void AbortWithOutOfMemory() noexcept;
  [[nodiscard]] ExecutionStatus Throw(Value payload);
  [[nodiscard]] ExecutionStatus Rethrow(Value exception) noexcept;
  [[nodiscard]] Value TakeException() noexcept;
  [[nodiscard]] Value pending_exception() const noexcept { return pending_exception_; }
  [[nodiscard]] bool HasException() const noexcept { return !pending_exception_.IsNull(); }

 private:
  friend class NativeInstanceScope;
  friend struct NativeMemoryLayout;
  NativeInstance* instance_;
  Isolate* isolate_;
  NativeTraceFrame* current_frame_ = nullptr;
  std::size_t frame_count_ = 0;
  Value pending_exception_;
  RootRange exception_root_;
  bool exception_allocation_failed_ = false;

  [[nodiscard]] ExecutionStatus CreateException(Value payload,
                                                ExecutionStatus status,
                                                std::string_view message);
  [[nodiscard]] std::span<const TracePosition> CapturePositions(
      std::span<TracePosition> storage) const noexcept;
};

// 切换到闭包创建时的实例；调用深度、根和异常仍属于同一次逻辑执行。
class NativeInstanceScope final {
 public:
  NativeInstanceScope(NativeExecution& execution, NativeInstance& instance) noexcept;
  ~NativeInstanceScope();
  NativeInstanceScope(const NativeInstanceScope&) = delete;
  NativeInstanceScope& operator=(const NativeInstanceScope&) = delete;
  NativeInstanceScope(NativeInstanceScope&&) = delete;
  NativeInstanceScope& operator=(NativeInstanceScope&&) = delete;

 private:
  NativeExecution* execution_;
  NativeInstance* previous_;
};

// 版本化符号使错误 ABI 的静态产物在链接时即被拒绝。
extern "C" std::uint8_t vela_native_v1_frame_storage(NativeExecution* execution,
                                                     std::uint64_t root_count,
                                                     std::uint64_t storage_bytes,
                                                     Value* stack_roots,
                                                     NativeFrame* frame,
                                                     NativeFrameAccess* access) noexcept;
extern "C" void vela_native_v1_frame_release(NativeFrame* frame) noexcept;
// 标量叶函数成功时无需逻辑帧；失败时补建逻辑帧捕获栈。
extern "C" std::uint8_t vela_native_v1_leaf_failure(NativeExecution* execution,
                                                    const TracePosition* position,
                                                    std::uint8_t status);
extern "C" std::uint8_t vela_native_v1_failure(NativeExecution* execution, std::uint8_t status);
extern "C" std::uint8_t vela_native_v1_has_exception(NativeExecution* execution) noexcept;
extern "C" void vela_native_v1_catch(NativeExecution* execution, NativeValue* result) noexcept;
extern "C" void vela_native_v1_exception_value(const NativeValue* exception,
                                               NativeValue* result) noexcept;
extern "C" std::uint8_t vela_native_v1_throw(NativeExecution* execution,
                                             const NativeValue* payload);
extern "C" std::uint8_t vela_native_v1_rethrow(NativeExecution* execution,
                                               const NativeValue* exception) noexcept;

}  // namespace vela::runtime
