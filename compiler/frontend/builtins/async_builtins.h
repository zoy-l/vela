#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

#include "base/async_operation.h"
#include "base/member.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/typing/semantic_model.h"

namespace vela::frontend {

struct AsyncMember final {
  TypeDescriptor::Kind receiver;
  MemberStorage storage;
  std::string_view name;
  AsyncOperation operation;
  MemberKind kind;
  std::array<std::string_view, 3> parameters;
  std::size_t required;
};

[[nodiscard]] std::span<const AsyncMember> AsyncMembers() noexcept;
[[nodiscard]] const AsyncMember* FindAsyncMember(TypeDescriptor::Kind receiver,
                                                 MemberStorage storage,
                                                 std::string_view name) noexcept;
// checker 先推导回调和载荷，再将最终类型投影到同一份参数名称、顺序与可选性契约。
[[nodiscard]] FunctionDescriptor MakeAsyncSignature(const AsyncMember& member,
                                                    const TypeDescriptor& result,
                                                    std::span<const TypeDescriptor> parameters);
// 工具用抽象 T/R 表示尚未提供的实参；只返回签名，不登记可执行类型。
[[nodiscard]] FunctionDescriptor DescribeAsyncMember(const AsyncMember& member,
                                                     const TypeDescriptor& element);

}  // namespace vela::frontend
