#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "base/callable.h"
#include "base/native_program_mode.h"
#include "compiler/ir/ir.h"

namespace vela::compiler::native {

struct CallEdge final {
  std::size_t caller = 0;
  std::size_t callee = 0;
};

// 同一调用图同时决定机器调用边界和代码复用依赖，避免失效规划与 lowering 分歧。
struct NativeCallPlan final {
  std::vector<std::size_t> components;
  std::vector<std::uint8_t> slot_targets;
  std::vector<std::vector<std::size_t>> dependencies;
  [[nodiscard]] bool IsEntrySlotRequired(CallEdge edge) const;
};

NativeCallPlan PlanNativeCalls(const ir::Program& program, NativeCodeTier tier);

// 只证明本函数创建且未覆写的 callable；索引对应输入 IR，不改变恢复位置。
std::vector<std::optional<CallableTarget>> FindDirectCalls(const ir::Function& function,
                                                           const ir::Program& program);

}  // namespace vela::compiler::native
