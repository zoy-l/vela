#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
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

class NativeFrame final {
 public:
  NativeFrame(Isolate& isolate, TracePosition position, NativeFrameSize size) noexcept;
  ~NativeFrame();
  NativeFrame(const NativeFrame&) = delete;
  NativeFrame& operator=(const NativeFrame&) = delete;
  NativeFrame(NativeFrame&&) = delete;
  NativeFrame& operator=(NativeFrame&&) = delete;

  [[nodiscard]] std::span<Value> roots() noexcept { return roots_.values(); }
  [[nodiscard]] bool IsAllocated() const noexcept {
    return roots_.IsAllocated() && (storage_bytes_ == 0 || storage_ != nullptr);
  }
  [[nodiscard]] void* storage() const noexcept { return storage_; }
  [[nodiscard]] TracePosition position() const noexcept { return position_; }
  void set_instruction(std::uint32_t instruction) noexcept { position_.instruction = instruction; }

 private:
  ValueBuffer roots_;
  RootRange root_range_;
  TracePosition position_;
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
  ~NativeExecution();
  NativeExecution(const NativeExecution&) = delete;
  NativeExecution& operator=(const NativeExecution&) = delete;
  NativeExecution(NativeExecution&&) = delete;
  NativeExecution& operator=(NativeExecution&&) = delete;

  [[nodiscard]] ExecutionStatus Enter(std::uint32_t function, NativeFrameSize size) noexcept;
  void Exit() noexcept;
  [[nodiscard]] std::size_t frame_count() const noexcept { return frame_count_; }
  [[nodiscard]] std::span<Value> current_roots() noexcept;
  [[nodiscard]] void* current_storage() const noexcept;
  [[nodiscard]] NativeInstance& instance() const noexcept { return *instance_; }
  [[nodiscard]] Isolate& isolate() const noexcept;
  void SetInstruction(std::uint32_t instruction) noexcept;
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
  NativeInstance* instance_;
  std::array<std::unique_ptr<NativeFrame>, kMaxCallFrames> frames_;
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
extern "C" std::uint8_t vela_native_v1_frame_enter(NativeExecution* execution,
                                                   std::uint64_t root_count,
                                                   std::uint32_t function,
                                                   std::uint64_t storage_bytes) noexcept;
extern "C" void vela_native_v1_frame_exit(NativeExecution* execution) noexcept;
extern "C" void* vela_native_v1_frame_storage(NativeExecution* execution) noexcept;
extern "C" void vela_native_v1_position(NativeExecution* execution,
                                        std::uint32_t instruction) noexcept;
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
