#pragma once

#include <cstdint>
#include <expected>
#include <span>

#include "base/callable.h"
#include "base/execution_status.h"
#include "runtime/async/future_composition.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_image.h"
#include "runtime/native/native_instance.h"
#include "runtime/value/callable.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {

[[nodiscard]] std::expected<HeapObject*, ExecutionStatus> AllocateNativeCallable(
    NativeExecution& execution,
    const ClassLayout& layout,
    std::span<const Value> captures = {});
[[nodiscard]] std::expected<void, ExecutionStatus> CaptureNativeReceiver(NativeExecution& execution,
                                                                         HeapObject& callable);
extern "C" std::uint8_t vela_native_v3_capture_receiver(NativeExecution* execution,
                                                        HeapObject* callable);

// 仅捕获 Isolate 与代码身份；回调的实例状态由 callable 的 managed 边保留。
[[nodiscard]] FutureCallbacks NativeFutureCallbacks(const NativeInstance& instance);

struct NativeResolvedCallable final {
  HeapObject* state = nullptr;
  ResolvedCallable target;
};

[[nodiscard]] std::expected<NativeResolvedCallable, ExecutionStatus> ResolveNativeCallable(
    const NativeInstance& caller,
    Value callable);

// 借用 callable 所保活版本的签名；调用者必须在使用期间保根 callable。
struct NativeCallableContract final {
  const CallSignature* signature = nullptr;
  std::span<const CallSignature::Parameter> parameters;
  std::span<const TypeConstraint> types;
  bool is_async = false;
};

[[nodiscard]] std::expected<NativeCallableContract, ExecutionStatus> InspectNativeCallable(
    const NativeInstance& caller,
    Value callable);

// 受信任的脚本驱动可调用内部声明；参数仍按真实签名校验并填充省略的可选实参。
[[nodiscard]] std::expected<Value, ExecutionStatus> InvokeNativeFunction(
    NativeExecution& execution,
    std::uint32_t function,
    std::span<const Value> arguments = {});
[[nodiscard]] std::expected<Value, ExecutionStatus> InvokeNativeCallable(
    NativeExecution& execution,
    Value callable,
    std::span<const Value> arguments = {});

// 公开命名/版本入口与 AOT 宿主包装共用参数检查；Value 参数在执行期间保根。
[[nodiscard]] std::expected<Value, ExecutionStatus> InvokeNativeEntry(NativeExecution& execution,
                                                                      std::uint32_t function,
                                                                      std::span<const Value> input);

namespace internal {

[[nodiscard]] std::expected<Value, ExecutionStatus> BoxNativeEnumResult(NativeExecution& execution,
                                                                        std::uint32_t type,
                                                                        Value value);

}  // namespace internal

// 参数已经过调用点绑定与验证；返回 runtime 表示，enum 结果在此完成盒装。
[[nodiscard]] inline std::expected<Value, ExecutionStatus> InvokeNativeTarget(
    NativeExecution& execution,
    std::uint32_t function,
    std::span<const NativeValue> arguments) {
  const auto& target = execution.instance().image().functions.values().subspan(function).front();
  NativeValue output;
  const auto status = static_cast<ExecutionStatus>(
      target.entry(&execution, arguments.data(), arguments.size(), &output));
  if (status != ExecutionStatus::Returned) {
    return std::unexpected(status);
  }
  const auto value = DecodeNativeValue(output);
  return (target.flags & kNativeEnumResult) != 0
             ? internal::BoxNativeEnumResult(execution, target.result_type, value)
             : std::expected<Value, ExecutionStatus>(value);
}

// indirect 参数区首槽预留给闭包环境或绑定接收者；显式实参从第二槽开始。
extern "C" std::uint8_t vela_native_v1_call_indirect(NativeExecution* execution,
                                                     const NativeValue* callable,
                                                     NativeValue* arguments,
                                                     std::uint64_t count,
                                                     NativeValue* result);
extern "C" std::uint8_t vela_native_v1_call_interface(NativeExecution* execution,
                                                      std::uint32_t selector,
                                                      const NativeValue* arguments,
                                                      std::uint64_t count,
                                                      NativeValue* result);
extern "C" std::uint8_t vela_native_v3_call_declaration(NativeExecution* execution,
                                                        std::uint64_t declaration,
                                                        const NativeValue* arguments,
                                                        std::uint64_t count,
                                                        NativeValue* result);
extern "C" std::uint8_t vela_native_v3_call_interface(NativeExecution* execution,
                                                      std::uint64_t selector,
                                                      const NativeValue* arguments,
                                                      std::uint64_t count,
                                                      NativeValue* result);

}  // namespace vela::runtime
