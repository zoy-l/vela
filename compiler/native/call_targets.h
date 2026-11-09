#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "base/callable.h"
#include "base/native_program_mode.h"
#include "compiler/ir/ir.h"
#include "compiler/native/native_roots.h"

namespace vela::compiler::native {

struct CallEdge final {
  std::size_t caller = 0;
  std::size_t callee = 0;
};

// 同一调用图同时决定机器调用边界和代码复用依赖，避免失效规划与 lowering 分歧。
struct NativeCallPlan final {
  std::vector<std::vector<std::size_t>> slot_calls;
  std::vector<std::vector<std::size_t>> dependencies;
  std::vector<std::vector<std::optional<CallableTarget>>> direct_calls;
  [[nodiscard]] bool IsEntrySlotRequired(CallEdge edge) const;
};

NativeCallPlan PlanNativeCalls(const ir::Program& program,
                               NativeCodeTier tier,
                               NativeDebugging debugging = NativeDebugging::Safepoints);

inline constexpr std::size_t kMaximumInlineInstructions = 64;

enum class LeafKind : std::uint8_t { None, Bounded, Guarded, Recursive };

[[nodiscard]] bool CanInlineSmallFunction(const ir::Function& function);
[[nodiscard]] LeafKind PlanNonrecursiveLeaf(const ir::Function& function,
                                            NativeDebugging debugging);

// 只证明本函数创建且未覆写的 callable；索引对应输入 IR，不改变恢复位置。
std::vector<std::optional<CallableTarget>> FindDirectCalls(const ir::Function& function,
                                                           const ir::Program& program);

}  // namespace vela::compiler::native
