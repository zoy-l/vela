#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <absl/container/flat_hash_set.h>

#include "compiler/ir/ir.h"

namespace vela::ir {

enum class Operand : std::uint8_t { Destination, Left, Right };

struct RegisterAccess final {
  bool defines_value = false;
  std::array<Operand, 3> uses = {};
  std::uint8_t use_count = 0;
  std::uint32_t argument_count = 0;
};

[[nodiscard]] auto AnalyzeRegisters(const Instruction& instruction,
                                    std::span<const Function> functions) -> RegisterAccess;
[[nodiscard]] auto ReadOperand(const Instruction& instruction, Operand operand) -> RegisterId;
void WriteOperand(Instruction& instruction, Operand operand, RegisterId value);

template <typename Visitor>
void ForEachUse(const Instruction& instruction,
                const RegisterAccess& access,
                const Visitor& visitor) {
  for (const auto operand : std::span(access.uses).first(access.use_count)) {
    visitor(ReadOperand(instruction, operand));
  }
  for (std::uint32_t index = 0; index < access.argument_count; ++index) {
    visitor(instruction.lhs + index);
  }
}

struct BasicBlock final {
  std::size_t begin = 0;
  std::size_t end = 0;
  std::vector<std::size_t> successors;
  std::vector<std::size_t> predecessors;
};

struct FlowGraph final {
  std::vector<BasicBlock> blocks;
  std::vector<std::size_t> instruction_blocks;
};

using RegisterSet = absl::flat_hash_set<RegisterId>;

struct BlockLiveness final {
  RegisterSet live_in;
  RegisterSet live_out;
};

enum class LivenessMode : std::uint8_t { AllInstructions, NecessaryInstructions };

[[nodiscard]] auto BuildFlowGraph(const Function& function) -> FlowGraph;
[[nodiscard]] auto AnalyzeLiveness(const Function& function,
                                   const FlowGraph& graph,
                                   std::span<const Function> functions,
                                   LivenessMode mode = LivenessMode::AllInstructions)
    -> std::vector<BlockLiveness>;
void CompactInstructions(Function& function, std::span<const std::uint8_t> keep);
[[nodiscard]] auto IsRemovableInstruction(Opcode opcode) -> bool;
[[nodiscard]] auto MayThrow(Opcode opcode) -> bool;

}  // namespace vela::ir
