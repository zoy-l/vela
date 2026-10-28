#include "compiler/aot/native_type_optimizer.h"

#include <cstdint>
#include <optional>

#include <absl/container/flat_hash_map.h>

#include "base/builtin.h"
#include "base/opcode.h"
#include "compiler/ir/analysis.h"

namespace vela::compiler::aot {

void ElideNativeResultBindings(ir::Program& program) {
  struct Fact {
    std::optional<std::int64_t> integer;
    std::optional<std::uint32_t> bound_type;
  };
  for (auto& function : program.functions) {
    const auto graph = ir::BuildFlowGraph(function);
    for (const auto& block : graph.blocks) {
      absl::flat_hash_map<ir::RegisterId, Fact> facts;
      for (auto index = block.begin; index < block.end; ++index) {
        auto& instruction = function.instructions.at(index);
        const auto access = ir::AnalyzeRegisters(instruction, program.functions);
        if (!access.defines_value) {
          continue;
        }
        Fact result;
        if (instruction.opcode == Opcode::LoadInt) {
          result.integer = instruction.immediate;
        } else if (instruction.opcode == Opcode::Move) {
          result = facts[instruction.lhs];
        } else if (instruction.opcode == Opcode::Call) {
          const auto& callee = program.functions.at(instruction.index);
          if ((callee.is_async || !callee.native_symbol.empty()) && callee.signature &&
              callee.return_type.kind == frontend::TypeDescriptor::Kind::Future) {
            result.bound_type = callee.signature->result;
          }
        } else if (instruction.opcode == Opcode::BuiltinCall &&
                   instruction.index == static_cast<std::uint32_t>(Builtin::BindRuntimeType)) {
          const auto input = facts[instruction.lhs];
          const auto type = facts[instruction.lhs + 1].integer;
          if (type) {
            result.bound_type = static_cast<std::uint32_t>(*type);
            if (input.bound_type == result.bound_type) {
              instruction = {.opcode = Opcode::Move,
                             .dst = instruction.dst,
                             .lhs = instruction.lhs,
                             .location = instruction.location,
                             .handler = instruction.handler};
            }
          }
        }
        facts.insert_or_assign(instruction.dst, result);
      }
    }
  }
}

}  // namespace vela::compiler::aot
