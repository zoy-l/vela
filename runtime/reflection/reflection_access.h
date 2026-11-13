#pragma once

#include <expected>
#include <span>
#include <string_view>

#include "base/declaration.h"
#include "base/execution_status.h"
#include "runtime/native/native_abi.h"
#include "runtime/value/value.h"

namespace vela::runtime {

struct NativeReflectionMember;
struct ReflectionHandle;

// 已解析成员借用当前 execution 实例的索引；调用方保根接收者和实参。
[[nodiscard]] std::expected<Value, ExecutionStatus> ReadReflectedMember(
    NativeExecution& execution,
    const NativeReflectionMember& member,
    Value receiver);
[[nodiscard]] std::expected<void, ExecutionStatus> WriteReflectedMember(
    NativeExecution& execution,
    const NativeReflectionMember& member,
    Value receiver,
    Value original);
[[nodiscard]] std::expected<Value, ExecutionStatus> InvokeReflectedMember(
    NativeExecution& execution,
    const NativeReflectionMember& member,
    Value receiver,
    std::span<const Value> values,
    std::span<const std::string_view> names);
// 脚本 CallArgument 数组在进入原生目标前完成绑定和快照；数组及接收者由调用方保根。
[[nodiscard]] std::expected<Value, ExecutionStatus> InvokeScriptReflectedMember(
    NativeExecution& execution,
    const NativeReflectionMember& member,
    Value receiver,
    const HeapObject& arguments,
    const ReflectionHandle& handle);

// arguments 是交替的 value/name NativeValue 窗口，已由生成调用帧完整保根。
[[nodiscard]] std::expected<Value, ExecutionStatus> InvokeScriptReflectedMember(
    NativeExecution& execution,
    const NativeReflectionMember& member,
    Value receiver,
    std::span<const NativeValue> arguments,
    const ReflectionHandle& handle);

[[nodiscard]] std::expected<Value, ExecutionStatus> ReadReflectedMember(NativeExecution& execution,
                                                                        DeclarationId declaration,
                                                                        Value receiver);
[[nodiscard]] std::expected<void, ExecutionStatus> WriteReflectedMember(NativeExecution& execution,
                                                                        DeclarationId declaration,
                                                                        Value receiver,
                                                                        Value original);
// values 与 names 为同一顺序、等长的稳定快照；空名称仅表示位置实参。
[[nodiscard]] std::expected<Value, ExecutionStatus> InvokeReflectedMember(
    NativeExecution& execution,
    DeclarationId declaration,
    Value receiver,
    std::span<const Value> values,
    std::span<const std::string_view> names);
[[nodiscard]] std::expected<Value, ExecutionStatus> ReadReflectedEnumMember(
    NativeExecution& execution,
    DeclarationId declaration);

}  // namespace vela::runtime
