#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
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

struct BoundArguments;
enum class NativeArgumentPresence : std::uint8_t { Absent, Included };

// 同步借用捕获方；失败必须在本次独立执行仍持有异常根时完成所有权转移。
struct NativeFailureObserver final {
  void* context;
  void (*capture)(void*, NativeExecution&, ExecutionStatus);
};

// 已证明有界且无 GC 的成功调用只持有小型上下文；普通执行和错误资源由冷路径拥有。
[[nodiscard]] std::expected<Value, ExecutionStatus> InvokeNativeArrayEntry(
    NativeInstance& instance,
    const NativeFunctionDescription& function,
    const ArrayLayout& array,
    std::span<const Value> input,
    NativeHostEntry selected_host,
    const NativeExecution* parent = nullptr,
    const NativeFailureObserver* failure = nullptr);

[[nodiscard]] std::expected<Value, ExecutionStatus> InvokeNativeEntry(
    NativeInstance& instance,
    std::uint32_t function,
    std::span<const Value> input,
    const NativeExecution* parent,
    const NativeFailureObserver* failure);

// 源实参与隐式接收者由调用方保根；适配器拥有缺省/rest 值、根槽和 ABI 缓冲，直到调用返回。
[[nodiscard]] std::expected<Value, ExecutionStatus> InvokeBoundNativeTarget(
    NativeExecution& execution,
    std::uint32_t function,
    const CallSignature& signature,
    const BoundArguments& bound,
    const HeapObject& arguments,
    std::optional<Value> receiver);
[[nodiscard]] std::expected<Value, ExecutionStatus> InvokeBoundNativeTarget(
    NativeExecution& execution,
    std::uint32_t function,
    const CallSignature& signature,
    const BoundArguments& bound,
    std::span<const Value> arguments,
    std::optional<Value> receiver,
    NativeArgumentPresence presence);

[[nodiscard]] std::expected<HeapObject*, ExecutionStatus> AllocateNativeCallable(
    NativeExecution& execution,
    const ClassLayout& layout,
    std::span<const Value> captures = {});
extern "C" std::uint8_t vela_native_v3_capture_receiver(NativeExecution* execution,
                                                        HeapObject* callable);

// 仅捕获 Isolate 与代码身份；回调的实例状态由 callable 的 managed 边保留。
[[nodiscard]] FutureCallbacks NativeFutureCallbacks(const NativeInstance& instance);

struct NativeResolvedCallable final {
  HeapObject* state = nullptr;
  ResolvedCallable target;
  const NativeFunctionDescription* function = nullptr;
};

[[nodiscard]] std::expected<NativeResolvedCallable, ExecutionStatus>
ResolveNativeMethod(const NativeInstance& caller, CallableTarget method, Value receiver);

[[nodiscard]] std::expected<NativeResolvedCallable, ExecutionStatus> ResolveNativeCallable(
    const NativeInstance& caller,
    Value callable);

// 调用者已保根函数值并进入其捕获版本。
[[nodiscard]] std::expected<Value, ExecutionStatus> InvokeResolvedNativeCallable(
    NativeExecution& execution,
    const NativeResolvedCallable& callback,
    std::span<const Value> arguments);

// 借用 callable 所保活版本的签名；调用者必须在使用期间保根 callable。
struct NativeCallableContract final {
  const CallSignature* signature = nullptr;
  std::span<const CallSignature::Parameter> parameters;
  std::span<const TypeConstraint> types;
  bool is_async = false;
  [[nodiscard]] bool HasRequiredParameters(std::size_t skipped = 0) const;
};

[[nodiscard]] std::expected<NativeCallableContract, ExecutionStatus> InspectNativeCallable(
    const NativeInstance& caller,
    Value callable);

[[nodiscard]] std::expected<Value, ExecutionStatus> InvokeNativeCallable(
    NativeExecution& execution,
    Value callable,
    std::span<const Value> arguments = {});

// 公开命名/版本入口与 AOT 宿主包装共用参数检查；Value 参数在执行期间保根。
[[nodiscard]] std::expected<Value, ExecutionStatus> InvokeNativeEntry(NativeExecution& execution,
                                                                      std::uint32_t function,
                                                                      std::span<const Value> input);

namespace internal {

[[nodiscard]] std::expected<Value, ExecutionStatus>
BoxNativeEnumResult(const NativeInstance& instance, std::uint32_t type, Value value);

}  // namespace internal

// 参数已经过调用点绑定与验证；返回 runtime 表示，enum 结果在此完成盒装。
// 小型适配层在调用点内联，消除中间 expected/Value 返回槽的封装与拷贝。
[[nodiscard]] inline std::expected<Value, ExecutionStatus> InvokeNativeTarget(
    NativeExecution& execution,
    const NativeFunctionDescription& target,
    std::span<const NativeValue> arguments) {
  const auto& instance = execution.instance();
  NativeValue output;
  const auto status = static_cast<ExecutionStatus>(
      target.entry(&execution, arguments.data(), arguments.size(), &output));
  if (status != ExecutionStatus::Returned) {
    return std::unexpected(status);
  }
  const auto value = DecodeNativeValue(output);
  return (target.flags & kNativeEnumResult) != 0
             ? internal::BoxNativeEnumResult(instance, target.result_type, value)
             : std::expected<Value, ExecutionStatus>(value);
}

[[nodiscard]] inline std::expected<Value, ExecutionStatus> InvokeNativeTarget(
    NativeExecution& execution,
    std::uint32_t function,
    std::span<const NativeValue> arguments) {
  return InvokeNativeTarget(execution, execution.instance().image().function(function), arguments);
}

// 受信任的脚本驱动可调用内部声明；参数仍按真实签名校验并填充省略的可选实参。
[[nodiscard]] inline std::expected<Value, ExecutionStatus> InvokeNativeFunction(
    NativeExecution& execution,
    std::uint32_t function,
    std::span<const Value> arguments = {}) {
  const auto functions = execution.instance().image().functions.values();
  if (function >= functions.size()) {
    return std::unexpected(ExecutionStatus::InvalidEntry);
  }
  const auto& description = functions.subspan(function).front();
  if (description.hidden_parameters != 0) {
    return std::unexpected(ExecutionStatus::InvalidEntry);
  }
  if (description.parameter_count == 0 && arguments.empty()) {
    // 无参数的命名入口已验证目标，无需构造闭包调用描述。
    return InvokeNativeTarget(execution, description, {});
  }
  return InvokeResolvedNativeCallable(
      execution, {.target = {.function = function}, .function = &description}, arguments);
}

// indirect 参数区首槽预留给闭包环境或绑定接收者；显式实参从第二槽开始。
extern "C" std::uint8_t vela_native_v1_call_indirect(NativeExecution* execution,
                                                     const NativeValue* callable,
                                                     NativeValue* arguments,
                                                     std::uint64_t count,
                                                     NativeValue* result);
extern "C" std::uint8_t vela_native_v1_call_virtual(NativeExecution* execution,
                                                    std::uint32_t selector,
                                                    const NativeValue* arguments,
                                                    std::uint64_t count,
                                                    NativeValue* result);
extern "C" std::uint8_t vela_native_v3_call_declaration(NativeExecution* execution,
                                                        std::uint64_t declaration,
                                                        const NativeValue* arguments,
                                                        std::uint64_t count,
                                                        NativeValue* result);
extern "C" std::uint8_t vela_native_v3_call_virtual(NativeExecution* execution,
                                                    std::uint64_t selector,
                                                    const NativeValue* arguments,
                                                    std::uint64_t count,
                                                    NativeValue* result);

}  // namespace vela::runtime
