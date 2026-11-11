#include "compiler/native/global_analysis.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include <llvm/ADT/BitVector.h>

#include "base/declaration.h"
#include "base/opcode.h"
#include "base/reflection.h"
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"
#include "compiler/native/code_generator.h"

namespace vela::compiler::native {
namespace {

struct GlobalFunction final {
  ir::FlowGraph graph;
  std::vector<std::uint8_t> reachable;
  std::vector<std::size_t> callers;
  llvm::BitVector initialized;
  std::vector<llvm::BitVector> block_outputs;
};

void TransferGlobal(const ir::Instruction& instruction,
                    std::span<const GlobalFunction> functions,
                    llvm::BitVector& initialized) {
  if (instruction.opcode == Opcode::GetGlobal || instruction.opcode == Opcode::SetGlobal) {
    initialized.set(instruction.index);
  } else if (instruction.opcode == Opcode::Call) {
    initialized |= functions[instruction.index].initialized;
  }
}

void InitializeBlockEntry(std::size_t index,
                          const GlobalFunction& facts,
                          std::span<const llvm::BitVector> outputs,
                          llvm::BitVector& ready) {
  if (index == 0) {
    ready.reset();
  } else {
    ready.set();
    for (const auto predecessor : facts.graph.blocks[index].predecessors) {
      if (facts.reachable[predecessor] != 0) {
        ready &= outputs[predecessor];
      }
    }
  }
}

void SolveBlockGlobalStates(const ir::Function& function,
                            GlobalFunction& facts,
                            std::span<const GlobalFunction> functions) {
  const auto& graph = facts.graph;
  auto& outputs = facts.block_outputs;
  std::vector<std::size_t> work;
  std::vector<std::uint8_t> queued = facts.reachable;
  llvm::BitVector ready(facts.initialized.size());
  for (std::size_t block = 0; block < graph.blocks.size(); ++block) {
    if (facts.reachable[block] != 0) {
      work.push_back(block);
    }
  }
  while (!work.empty()) {
    const auto index = work.back();
    work.pop_back();
    queued[index] = 0;
    const auto& block = graph.blocks[index];
    InitializeBlockEntry(index, facts, outputs, ready);
    for (auto instruction = block.begin; instruction < block.end; ++instruction) {
      TransferGlobal(function.instructions[instruction], functions, ready);
    }
    if (ready == outputs[index]) {
      continue;
    }
    outputs[index].swap(ready);
    for (const auto successor : block.successors) {
      if (queued[successor] == 0) {
        queued[successor] = 1;
        work.push_back(successor);
      }
    }
  }
}

llvm::BitVector InitializedOnReturn(const ir::Function& function, const GlobalFunction& facts) {
  const auto& outputs = facts.block_outputs;
  const auto& graph = facts.graph;
  llvm::BitVector result(facts.initialized.size(), true);
  for (std::size_t index = 0; index < graph.blocks.size(); ++index) {
    const auto& block = graph.blocks[index];
    if (facts.reachable[index] != 0 &&
        function.instructions[block.end - 1].opcode == Opcode::Return) {
      result &= outputs[index];
    }
  }
  return result;
}

std::vector<std::uint8_t> FindInitializedAccesses(const ir::Function& function,
                                                  const GlobalFunction& facts,
                                                  std::span<const GlobalFunction> functions) {
  std::vector<std::uint8_t> stores(function.instructions.size());
  const auto& outputs = facts.block_outputs;
  llvm::BitVector ready(facts.initialized.size());
  for (std::size_t index = 0; index < facts.graph.blocks.size(); ++index) {
    if (facts.reachable[index] == 0) {
      continue;
    }
    InitializeBlockEntry(index, facts, outputs, ready);
    const auto& block = facts.graph.blocks[index];
    for (auto position = block.begin; position < block.end; ++position) {
      const auto& instruction = function.instructions[position];
      const auto is_global_access =
          instruction.opcode == Opcode::GetGlobal || instruction.opcode == Opcode::SetGlobal;
      stores[position] =
          static_cast<std::uint8_t>(is_global_access && ready.test(instruction.index));
      TransferGlobal(instruction, functions, ready);
    }
  }
  return stores;
}

}  // namespace

namespace {

void AccountReflectedGlobalWrites(const ir::Program& program,
                                  std::vector<std::uint8_t>& readonly_slots) {
  for (const auto& member : program.reflection_members) {
    if (member.kind == reflection::MemberKind::Field &&
        (member.flags & (reflection::kRegistered | reflection::kStatic | reflection::kPrivate)) ==
            (reflection::kRegistered | reflection::kStatic) &&
        (program.global_flags[member.target] & static_cast<std::uint32_t>(GlobalFlag::Constant)) ==
            0) {
      readonly_slots[member.target] = 0;
    }
  }
}

}  // namespace

GlobalAccessFacts AnalyzeGlobalAccess(const ir::Program& program, NativeCodePlacement placement) {
  if (!IsClosedLinkedProgram(program.native_mode, placement)) {
    return {};
  }
  const auto count = static_cast<unsigned>(program.global_initializers.size());
  GlobalAccessFacts result{
      .readonly_slots = std::vector<std::uint8_t>(count, 1),
      .initialized_on_return = std::vector<std::vector<std::uint32_t>>(program.functions.size()),
      .initialized_accesses = std::vector<std::vector<std::uint8_t>>(program.functions.size()),
  };
  AccountReflectedGlobalWrites(program, result.readonly_slots);
  std::vector<GlobalFunction> functions(program.functions.size());
  std::vector<std::size_t> work;
  std::vector<std::uint8_t> queued(functions.size());
  for (std::size_t index = 0; index < functions.size(); ++index) {
    const auto& function = program.functions[index];
    auto& facts = functions[index];
    const auto eligible =
        ir::CanAnalyzeSynchronousFunction(function) && !function.instructions.empty();
    facts.initialized.resize(count, eligible);
    if (eligible) {
      facts.graph = ir::BuildFlowGraph(function);
      facts.reachable = ir::FindReachableBlocks(facts.graph);
      facts.block_outputs.assign(facts.graph.blocks.size(), llvm::BitVector(count, true));
      work.push_back(index);
      queued[index] = 1;
    }
    for (const auto& instruction : function.instructions) {
      if (instruction.opcode == Opcode::SetGlobal) {
        result.readonly_slots[instruction.index] = 0;
      } else if (instruction.opcode == Opcode::Call) {
        functions[instruction.index].callers.push_back(index);
      }
    }
  }
  // 摘要约束的是正常返回。从全集递减才能证明递归基例建立、递归分支继承的状态。
  while (!work.empty()) {
    const auto index = work.back();
    work.pop_back();
    queued[index] = 0;
    const auto& function = program.functions[index];
    auto& facts = functions[index];
    SolveBlockGlobalStates(function, facts, functions);
    auto initialized = InitializedOnReturn(function, facts);
    if (initialized == facts.initialized) {
      continue;
    }
    facts.initialized = std::move(initialized);
    for (const auto caller : facts.callers) {
      if (!functions[caller].graph.blocks.empty() && queued[caller] == 0) {
        queued[caller] = 1;
        work.push_back(caller);
      }
    }
  }
  for (std::size_t index = 0; index < functions.size(); ++index) {
    const auto& function = program.functions[index];
    const auto& facts = functions[index];
    result.initialized_accesses[index] = FindInitializedAccesses(function, facts, functions);
    for (const auto slot : facts.initialized.set_bits()) {
      result.initialized_on_return[index].push_back(static_cast<std::uint32_t>(slot));
    }
  }
  return result;
}

}  // namespace vela::compiler::native
