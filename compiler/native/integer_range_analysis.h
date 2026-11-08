#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <llvm/IR/ConstantRange.h>

#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"

namespace vela::compiler::native {

struct IntegerRangeFacts final {
  bool reachable = false;
  std::vector<llvm::ConstantRange> parameters;
  std::vector<llvm::ConstantRange> indices;
  std::vector<llvm::ConstantRange> writes;
  std::vector<std::uint8_t> no_overflow;
};

// roots 的参数来自完整类型域；其余参数只合并直接调用的实参。返回事实只适用于
// 该调用域中的函数体，不能用于宿主可单独调用的通用入口。
// 两轮元素反馈共享阈值、CFG 和寄存器分析；收窄元素域时重置格状态，不能沿用旧宽域。
// program、roots 和 accesses 覆盖分析对象的生命周期；Run 返回的事实借用至下一次 Run。
class IntegerRangeAnalysis final {
 public:
  IntegerRangeAnalysis(const ir::Program& program,
                       std::span<const std::uint8_t> roots,
                       std::span<const std::vector<ir::RegisterAccess>> accesses);
  [[nodiscard]] std::span<IntegerRangeFacts> Run(
      std::span<const std::vector<std::uint64_t>> array_bits = {});

 private:
  using State = std::vector<llvm::ConstantRange>;
  struct FunctionFlow final {
    ir::FlowGraph graph;
    std::vector<ir::RegisterDefinition> definitions;
    std::vector<State> entries;
    std::vector<std::uint8_t> reachable;
    std::vector<std::size_t> work;
    std::vector<std::uint8_t> queued;
    State scratch;
  };
  struct EdgeRanges final {
    ir::RegisterId lhs = 0;
    ir::RegisterId rhs = 0;
    llvm::ConstantRange left;
    llvm::ConstantRange right;
  };

  void MergeCall(const ir::Instruction& instruction, std::span<const llvm::ConstantRange> state);
  void Propagate(std::size_t index);
  void Solve(std::size_t index);
  void MergeEdge(std::size_t function_index,
                 const ir::BasicBlock& block,
                 const State& state,
                 std::size_t successor);
  void Transfer(std::size_t function, std::size_t position, State& state) const;
  [[nodiscard]] std::optional<EdgeRanges> RefineEdge(std::size_t function_index,
                                                     const ir::BasicBlock& block,
                                                     std::size_t successor,
                                                     const State& state) const;
  void Publish(std::size_t index);

  const ir::Program* program_;
  std::span<const std::uint8_t> roots_;
  std::span<const std::vector<ir::RegisterAccess>> accesses_;
  std::span<const std::vector<std::uint64_t>> array_bits_;
  std::vector<std::int64_t> thresholds_;
  std::vector<IntegerRangeFacts> facts_;
  std::vector<std::optional<FunctionFlow>> flows_;
  std::vector<std::size_t> work_;
  std::vector<std::uint8_t> queued_;
};

}  // namespace vela::compiler::native
