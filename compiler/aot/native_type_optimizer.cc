#include "compiler/aot/native_type_optimizer.h"

#include <cstdint>
#include <optional>

#include <absl/container/flat_hash_map.h>

#include "base/builtin.h"
#include "base/opcode.h"
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"

namespace vela::compiler::aot {
namespace {

struct Fact {
  std::optional<std::int64_t> integer;
  std::optional<std::uint32_t> bound_type;
};

Fact LookupFact(const absl::flat_hash_map<ir::RegisterId, Fact>& facts, ir::RegisterId reg) {
  const auto found = facts.find(reg);
  return found == facts.end() ? Fact{} : found->second;
}

Fact ResultFact(ir::Instruction& instruction,
                const ir::Program& program,
                absl::flat_hash_map<ir::RegisterId, Fact>& facts) {
  Fact result;
  switch (instruction.opcode) {
    case Opcode::LoadInt:
      result.integer = instruction.immediate;
      return result;
    case Opcode::Move:
      return LookupFact(facts, instruction.lhs);
    case Opcode::Call: {
      const auto& callee = program.functions.at(instruction.index);
      if ((callee.is_async || !callee.native_symbol.empty()) && callee.signature &&
          callee.return_type.kind == frontend::TypeDescriptor::Kind::Future) {
        result.bound_type = callee.signature->result;
      }
      return result;
    }
    case Opcode::BuiltinCall:
      break;
    default:
      return result;
  }
  if (instruction.index != static_cast<std::uint32_t>(Builtin::BindRuntimeType)) {
    return result;
  }
  const auto input = LookupFact(facts, instruction.lhs);
  const auto type = LookupFact(facts, instruction.lhs + 1).integer;
  if (!type) {
    return result;
  }
  result.bound_type = static_cast<std::uint32_t>(*type);
  if (input.bound_type == result.bound_type) {
    instruction = {
        .opcode = Opcode::Move,
        .dst = instruction.dst,
        .lhs = instruction.lhs,
        .location = instruction.location,
        .handler = instruction.handler,
    };
  }
  return result;
}

}  // namespace

void ElideNativeResultBindings(ir::Program& program) {
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
        const auto result = ResultFact(instruction, program, facts);
        facts.insert_or_assign(instruction.dst, result);
      }
    }
  }
}

}  // namespace vela::compiler::aot
