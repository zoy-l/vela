#include "compiler/native/borrow_analysis.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "base/native_program_mode.h"
#include "base/opcode.h"
#include "compiler/frontend/type_checker.h"
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"
#include "compiler/native/escape_analysis.h"
#include "compiler/native/escape_dataflow.h"

namespace vela::compiler::native {
namespace {

using namespace internal;
using Kind = frontend::TypeDescriptor::Kind;

std::optional<ir::ClassId> BorrowedType(const ir::Program& program, const ir::Function& function) {
  if (function.parameter_count == 0 || function.is_async || !function.native_symbol.empty() ||
      function.is_generated_constructor || function.instructions.empty()) {
    return std::nullopt;
  }
  const auto& parameter = function.value_types.front();
  if (parameter.kind != Kind::Class || parameter.nullable) {
    return std::nullopt;
  }
  for (std::size_t index = 1; index < function.parameter_count; ++index) {
    const auto kind = function.value_types.at(index).kind;
    if (kind == Kind::Class || kind == Kind::Interface || kind == Kind::Dynamic ||
        kind == Kind::Function) {
      return std::nullopt;
    }
  }
  const auto found = std::ranges::find(program.classes, parameter.name, &ir::Class::name);
  if (found == program.classes.end() || found->kind != ir::Class::Kind::Instance ||
      found->callable || found->field_types.empty()) {
    return std::nullopt;
  }
  return static_cast<ir::ClassId>(found - program.classes.begin());
}

// 此入口不传播栈记录。字段本身可以保存真实托管引用，但不能写入参数的虚拟身份。
bool IsBorrowedUse(const EscapePlan& plan,
                   const ir::Program& program,
                   const ir::Instruction& instruction,
                   const State& values) {
  bool has_alias = false;
  bool ambiguous = false;
  ir::ForEachUse(instruction, ir::AnalyzeRegisters(instruction, program.functions),
                 [&](ir::RegisterId reg) {
                   const auto& value = values.at(reg);
                   has_alias = has_alias || value.objects != 0;
                   ambiguous = ambiguous || (value.objects != 0 && !Object(value));
                 });
  if (!has_alias) {
    return true;
  }
  return !ambiguous && !NeedsHeap(plan, program, instruction, values) &&
         (instruction.opcode != Opcode::SetField || values.at(instruction.lhs).objects == 0);
}

ir::Instruction BorrowedInstruction(const EscapePlan& plan,
                                    const ir::Program& program,
                                    const ir::Instruction& original,
                                    const State& values,
                                    ir::RegisterId marker) {
  auto instruction = original;
  const auto left = Object(values.at(original.lhs));
  const auto destination = Object(values.at(original.dst));
  if ((original.opcode == Opcode::GetField && left) ||
      (original.opcode == Opcode::SetField && destination)) {
    instruction.opcode = Opcode::Move;
    if (original.opcode == Opcode::GetField) {
      instruction.lhs = plan.borrowed_fields.at(original.index);
    } else {
      instruction.dst = plan.borrowed_fields.at(original.index);
    }
  } else if (original.opcode == Opcode::IsType && left) {
    instruction.opcode = Opcode::LoadBool;
    instruction.immediate =
        static_cast<std::int64_t>(MatchesType(program, plan.allocations.at(*left), original.index));
  } else if ((original.opcode == Opcode::EqualValue || original.opcode == Opcode::NotEqualValue) &&
             left) {
    instruction.opcode = Opcode::LoadBool;
    instruction.immediate = static_cast<std::int64_t>(original.opcode == Opcode::EqualValue);
  } else if ((original.opcode == Opcode::Move || original.opcode == Opcode::CheckedCast ||
              original.opcode == Opcode::JumpIfNull) &&
             left) {
    instruction.opcode = Opcode::LoadNull;
    if (original.opcode == Opcode::JumpIfNull) {
      instruction.dst = marker;
    }
  }
  return instruction;
}

std::optional<BorrowedFunction> PlanBorrow(const ir::Program& program,
                                           const ir::Function& source,
                                           ir::ClassId type) {
  EscapePlan plan{
      .function = source,
      .origins = {},
      .allocations = {},
      .checkpoints = {},
      .borrowable = {},
      .borrowed_calls = {},
      .borrowed_fields = {},
  };
  const auto& fields = program.classes.at(type).field_types;
  const auto state_size = source.register_count + fields.size() + 2;
  const auto graph = ir::BuildFlowGraph(source);
  constexpr std::size_t kMaximumBorrowStateCells = std::size_t{1024} * 1024;
  if (state_size * graph.blocks.size() > kMaximumBorrowStateCells) {
    return std::nullopt;
  }
  plan.allocations.push_back({
      .type = type,
      .fields = std::vector<ir::RegisterId>(fields.size()),
      .state_offset = source.register_count,
  });
  State entry(state_size);
  entry.front() = {.objects = 1, .other = false, .initialized = true};
  for (std::size_t field = 0; field < fields.size(); ++field) {
    entry.at(source.register_count + field).initialized = true;
    plan.borrowed_fields.push_back(plan.function.register_count++);
    plan.function.value_types.push_back(fields.at(field));
  }
  const auto marker = plan.function.register_count++;
  plan.function.value_types.push_back({.kind = Kind::Null, .name = "null"});
  const std::vector<std::optional<std::size_t>> allocations(source.instructions.size());
  const auto states =
      Solve(source, program, graph, allocations, plan, state_size, std::move(entry));
  for (std::size_t index = 0; index < graph.blocks.size(); ++index) {
    auto values = states.at(index).reached ? states.at(index).values : State(state_size);
    const auto& block = graph.blocks.at(index);
    for (auto position = block.begin; position < block.end; ++position) {
      const auto& original = source.instructions.at(position);
      if (!IsBorrowedUse(plan, program, original, values)) {
        return std::nullopt;
      }
      plan.function.instructions.at(position) =
          BorrowedInstruction(plan, program, original, values, marker);
      Transfer(original, ir::AnalyzeRegisters(original, program.functions), std::nullopt, plan,
               program, values);
    }
  }
  for (std::size_t index = 0; index < source.instructions.size(); ++index) {
    plan.origins.push_back(index);
  }
  plan.allocations.clear();
  return BorrowedFunction{.type = type, .plan = std::move(plan)};
}

}  // namespace

std::vector<std::optional<BorrowedFunction>> PlanBorrows(const ir::Program& program) {
  std::vector<std::optional<BorrowedFunction>> result(program.functions.size());
  if (program.native_mode != NativeProgramMode::Closed) {
    return result;
  }
  for (std::size_t index = 0; index < program.functions.size(); ++index) {
    const auto& function = program.functions.at(index);
    if (const auto type = BorrowedType(program, function)) {
      result.at(index) = PlanBorrow(program, function, *type);
    }
  }
  return result;
}

}  // namespace vela::compiler::native
