#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "compiler/ir/ir.h"
#include "compiler/native/function_emitter.h"

namespace vela::compiler::native {

// 同一程序的入口族由声明阶段统一持有，后续规划不能交换基线、优化与范围特化的顺序。
struct FunctionDeclarations final {
  std::vector<GeneratedFunction> baseline;
  std::vector<GeneratedFunction> optimized;
  std::vector<GeneratedFunction> specialized;
  std::vector<std::optional<ir::ClassId>> borrowable;

  [[nodiscard]] bool HasRangeSpecialization(std::size_t index) const {
    return !specialized.empty() && specialized[index].body != optimized[index].body;
  }
};

}  // namespace vela::compiler::native
