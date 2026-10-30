#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>
#include "base/native_program_mode.h"
#include "base/opcode.h"
#include "compiler/frontend/type_checker.h"
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"
#include "compiler/native/escape_analysis.h"
#include "compiler/native/escape_dataflow.h"
namespace vela::compiler::native::internal {
namespace {
std::optional<bool> KnownIdentity(const EscapePlan& plan,
                                  const ir::Instruction& instruction,
                                  const State& values) {
  if (instruction.opcode != Opcode::EqualValue && instruction.opcode != Opcode::NotEqualValue) {
    return std::nullopt;
  }
  const auto left = Object(values.at(instruction.lhs));
  const auto right = Object(values.at(instruction.rhs));
  if (!left || !right || (!IsVirtual(plan, *left) && !IsVirtual(plan, *right))) {
    return std::nullopt;
  }
  // 虚拟候选已排除重叠迭代；与另一个分配来源的堆实例必然身份不同。
  return (*left == *right) == (instruction.opcode == Opcode::EqualValue);
}

class ScalarRewriter final {
 public:
  ScalarRewriter(EscapePlan& plan,
                 const ir::Program& program,
                 std::span<const std::vector<ir::RegisterId>> live)
      : plan_(&plan),
        program_(&program),
        live_(live),
        original_(std::move(plan.function.instructions)),
        targets_(original_.size() + 1) {
    if (program_->native_mode == NativeProgramMode::Reloadable) {
      plan_->checkpoints.resize(original_.size());
    }
    for (std::size_t node = 0; node < plan_->allocations.size(); ++node) {
      auto& allocation = plan_->allocations.at(node);
      if (allocation.decision != AllocationDecision::ScalarReplacement) {
        continue;
      }
      const auto& type = program_->classes.at(allocation.type);
      const auto& types = type.field_types;
      for (std::size_t field = 0; field < types.size(); ++field) {
        allocation.fields.at(field) = AddValue(types.at(field));
      }
      allocation.reference = AddValue({
          .kind = frontend::TypeDescriptor::Kind::Class,
          .name = type.name + '?',
          .nullable = true,
      });
      plan_->reference_count = node + 1;
    }
    if (!plan_->checkpoints.empty()) {
      marker_ = AddValue({.kind = frontend::TypeDescriptor::Kind::Null, .name = "null"});
    }
  }

  void Run(const ir::FlowGraph& graph,
           std::span<const BlockState> blocks,
           std::span<const std::optional<std::size_t>> allocations) {
    for (std::size_t index = 0; index < graph.blocks.size(); ++index) {
      const auto& state = blocks.subspan(index).front();
      auto values = state.reached ? state.values : State(blocks.front().values.size());
      const auto& block = graph.blocks.at(index);
      for (auto position = block.begin; position < block.end; ++position) {
        position_ = position;
        targets_.at(position) = plan_->function.instructions.size();
        const auto& instruction = original_.at(position);
        RecordCheckpoint(values);
        RewriteInstruction(instruction, allocations.subspan(position).front(), values);
        if (!plan_->checkpoints.empty() &&
            plan_->function.instructions.size() == targets_.at(position)) {
          Emit({.opcode = Opcode::LoadNull, .dst = marker_, .location = {}});
        }
        Transfer(instruction, ir::AnalyzeRegisters(instruction, program_->functions),
                 allocations.subspan(position).front(), *plan_, *program_, values);
      }
    }
    targets_.back() = plan_->function.instructions.size();
    for (const auto& [position, target] : branches_) {
      Patch(position, targets_.at(target));
    }
    for (auto& handler : plan_->function.handlers) {
      handler.target = static_cast<std::uint32_t>(targets_.at(handler.target));
    }
  }

 private:
  void RecordCheckpoint(const State& values) {
    if (plan_->checkpoints.empty()) {
      return;
    }
    auto& checkpoint = plan_->checkpoints.at(position_);
    std::uint64_t objects = 0;
    for (const auto reg : live_.subspan(position_).front()) {
      const auto object = ScalarObject(*plan_, values.at(reg));
      if (object) {
        checkpoint.aliases.push_back({.reg = reg, .allocation = *object});
        objects |= std::uint64_t{1} << *object;
      } else {
        checkpoint.required.push_back(reg);
      }
    }
    auto pending = Reachable(*plan_, values, objects);
    while (pending != 0) {
      const auto node = static_cast<std::size_t>(std::countr_zero(pending));
      pending &= pending - 1;
      if (!IsVirtual(*plan_, node)) {
        continue;
      }
      const auto& allocation = plan_->allocations.at(node);
      const auto& type = program_->classes.at(allocation.type);
      EscapeObjectSnapshot snapshot{.allocation = node, .fields = {}};
      checkpoint.required.push_back(allocation.reference);
      for (std::size_t field = 0; field < allocation.fields.size(); ++field) {
        const auto& value = values.at(allocation.state_offset + field);
        const auto child = ScalarObject(*plan_, value);
        snapshot.fields.push_back({
            .declaration = type.fields.at(field).declaration,
            .value = allocation.fields.at(field),
            .object = child,
            .initialized = value.initialized,
        });
        if (!child) {
          checkpoint.required.push_back(allocation.fields.at(field));
        }
      }
      checkpoint.objects.push_back(std::move(snapshot));
    }
    std::ranges::sort(checkpoint.required);
    const auto duplicate = std::ranges::unique(checkpoint.required);
    checkpoint.required.erase(duplicate.begin(), duplicate.end());
  }

  ir::RegisterId AddValue(const frontend::TypeDescriptor& type) {
    auto& function = plan_->function;
    function.value_types.push_back(type);
    return function.register_count++;
  }

  std::size_t Emit(ir::Instruction instruction) {
    const auto& source = original_.at(position_);
    instruction.location = source.location;
    instruction.handler = source.handler;
    auto& code = plan_->function.instructions;
    const auto position = code.size();
    code.push_back(std::move(instruction));
    plan_->origins.push_back(position_);
    plan_->borrowed_calls.emplace_back(std::nullopt);
    return position;
  }

  void Patch(std::size_t position, std::size_t target) {
    plan_->function.instructions.at(position).index = static_cast<std::uint32_t>(target);
  }

  void Materialize(std::size_t object, const State& values) {
    const auto bit = std::uint64_t{1} << object;
    if ((MustHeap(values) & bit) != 0) {
      return;
    }
    const auto reference = plan_->allocations.at(object).reference;
    const auto missing = Emit({.opcode = Opcode::JumpIfNull, .lhs = reference, .location = {}});
    const auto ready = Emit({.opcode = Opcode::Jump, .location = {}});
    Patch(missing, plan_->function.instructions.size());
    // 临时引用只在完整填充后提交；OOM 的异常后继仍保有原来的字段状态。
    const auto graph = Reachable(*plan_, values, bit);
    const auto group_start = plan_->function.instructions.size();
    std::vector<ir::RegisterId> temporaries(plan_->allocations.size());
    auto pending = graph;
    while (pending != 0) {
      const auto node = static_cast<std::size_t>(std::countr_zero(pending));
      pending &= pending - 1;
      if (!IsVirtual(*plan_, node)) {
        continue;
      }
      auto& allocation = plan_->allocations.at(node);
      allocation.materialized = true;
      const auto temporary = AddValue(plan_->function.value_types.at(allocation.reference));
      temporaries.at(node) = temporary;
      Emit({
          .opcode = Opcode::NewInstance,
          .dst = temporary,
          .index = allocation.type,
          .location = {},
      });
    }
    pending = graph;
    while (pending != 0) {
      const auto node = static_cast<std::size_t>(std::countr_zero(pending));
      pending &= pending - 1;
      if (!IsVirtual(*plan_, node)) {
        continue;
      }
      const auto& allocation = plan_->allocations.at(node);
      for (std::size_t field = 0; field < allocation.fields.size(); ++field) {
        const auto child = ScalarObject(*plan_, values.at(allocation.state_offset + field));
        Emit({
            .opcode = Opcode::SetField,
            .dst = temporaries.at(node),
            .lhs = child ? temporaries.at(*child) : allocation.fields.at(field),
            .index = static_cast<std::uint32_t>(field),
            .location = {},
            .field_declaration =
                program_->native_mode == NativeProgramMode::Reloadable
                    ? std::optional(
                          program_->classes.at(allocation.type).fields.at(field).declaration)
                    : std::nullopt,
        });
      }
    }
    pending = graph;
    while (pending != 0) {
      const auto node = static_cast<std::size_t>(std::countr_zero(pending));
      pending &= pending - 1;
      if (!IsVirtual(*plan_, node)) {
        continue;
      }
      const auto& allocation = plan_->allocations.at(node);
      Emit({
          .opcode = Opcode::Move,
          .dst = allocation.reference,
          .lhs = temporaries.at(node),
          .location = {},
      });
      for (const auto field : allocation.fields) {
        Emit({.opcode = Opcode::LoadNull, .dst = field, .location = {}});
      }
    }
    if (program_->native_mode == NativeProgramMode::Reloadable) {
      auto& first = plan_->function.instructions.at(group_start);
      first.construction = ir::Construction{
          .type = program_->classes.at(first.index).declaration,
          .supplied = {},
          .generated = false,
      };
      plan_->function.instructions.back().completes_construction = true;
    }
    Patch(ready, plan_->function.instructions.size());
  }

  void Spill(ir::RegisterId reg, const State& values) {
    if (const auto object = ScalarObject(*plan_, values.at(reg))) {
      Materialize(*object, values);
      Emit({
          .opcode = Opcode::Move,
          .dst = reg,
          .lhs = plan_->allocations.at(*object).reference,
          .location = {},
      });
    }
  }

  void ConditionalField(ir::Instruction instruction, std::size_t receiver) {
    const auto& allocation = plan_->allocations.at(receiver);
    const auto missing =
        Emit({.opcode = Opcode::JumpIfNull, .lhs = allocation.reference, .location = {}});
    auto scalar = instruction;
    if (instruction.opcode == Opcode::GetField) {
      instruction.lhs = allocation.reference;
      scalar.lhs = allocation.fields.at(instruction.index);
    } else {
      instruction.dst = allocation.reference;
      scalar.dst = allocation.fields.at(instruction.index);
    }
    Emit(instruction);
    const auto ready = Emit({.opcode = Opcode::Jump, .location = {}});
    Patch(missing, plan_->function.instructions.size());
    scalar.opcode = Opcode::Move;
    Emit(scalar);
    Patch(ready, plan_->function.instructions.size());
  }

  void RewriteField(ir::Instruction instruction, std::size_t receiver, const State& values) {
    if ((MayHeap(values) & (std::uint64_t{1} << receiver)) != 0) {
      ConditionalField(instruction, receiver);
      return;
    }
    const auto& allocation = plan_->allocations.at(receiver);
    if (instruction.opcode == Opcode::GetField) {
      if (ScalarObject(*plan_, values.at(allocation.state_offset + instruction.index))) {
        return;
      }
      instruction.opcode = Opcode::Move;
      instruction.lhs = allocation.fields.at(instruction.index);
    } else {
      instruction.opcode =
          ScalarObject(*plan_, values.at(instruction.lhs)) ? Opcode::LoadNull : Opcode::Move;
      instruction.dst = allocation.fields.at(instruction.index);
    }
    Emit(instruction);
  }

  void RewriteInstruction(ir::Instruction instruction,
                          std::optional<std::size_t> allocation,
                          const State& values) {
    const auto left = ScalarObject(*plan_, values.at(instruction.lhs));
    if (const auto object = BorrowedReceiver(*plan_, instruction, values)) {
      plan_->allocations.at(*object).stack_record = true;
      Emit({.opcode = Opcode::LoadNull, .dst = instruction.lhs, .location = {}});
      const auto call = Emit(instruction);
      plan_->borrowed_calls.at(call) = *object;
      return;
    }
    if (instruction.opcode == Opcode::GetField || instruction.opcode == Opcode::SetField) {
      if (const auto receiver = VirtualReceiver(*plan_, instruction, values)) {
        RewriteField(instruction, *receiver, values);
        return;
      }
    }
    if (allocation &&
        plan_->allocations.at(*allocation).decision == AllocationDecision::ScalarReplacement) {
      const auto& object = plan_->allocations.at(*allocation);
      Emit({
          .opcode = Opcode::LoadNull,
          .dst = object.reference,
          .location = {},
          .construction = instruction.construction,
      });
      for (const auto field : object.fields) {
        Emit({.opcode = Opcode::LoadNull, .dst = field, .location = {}});
      }
      return;
    }
    if ((instruction.opcode == Opcode::Move || instruction.opcode == Opcode::JumpIfNull) && left) {
      return;
    }
    if (instruction.opcode == Opcode::IsType && left) {
      instruction.opcode = Opcode::LoadBool;
      instruction.immediate = static_cast<std::int64_t>(
          MatchesType(*program_, plan_->allocations.at(*left), instruction.index));
      Emit(instruction);
      return;
    }
    if (instruction.opcode == Opcode::CheckedCast && left &&
        MatchesType(*program_, plan_->allocations.at(*left), instruction.index)) {
      return;
    }
    if (instruction.opcode == Opcode::Call && left &&
        IsTrivialConstructor(program_->functions.at(instruction.index))) {
      // 空生成构造不读取 receiver。保留实际调用的逻辑帧、栈检查和失败边，
      // 仅为未使用的私有参数提供 null；虚拟身份不会进入对象 ABI。
      Emit({.opcode = Opcode::LoadNull, .dst = instruction.lhs, .location = {}});
      Emit(instruction);
      return;
    }
    if (const auto comparison = KnownIdentity(*plan_, instruction, values)) {
      instruction.immediate = static_cast<std::int64_t>(*comparison);
      instruction.opcode = Opcode::LoadBool;
      Emit(instruction);
      return;
    }
    const auto access = ir::AnalyzeRegisters(instruction, program_->functions);
    ir::ForEachUse(instruction, access, [&](ir::RegisterId reg) { Spill(reg, values); });
    if (instruction.opcode == Opcode::Jump || instruction.opcode == Opcode::JumpIfFalse ||
        instruction.opcode == Opcode::JumpIfNull) {
      branches_.emplace_back(plan_->function.instructions.size(), instruction.index);
    }
    Emit(instruction);
  }

  EscapePlan* plan_;
  const ir::Program* program_;
  std::span<const std::vector<ir::RegisterId>> live_;
  std::vector<ir::Instruction> original_;
  std::vector<std::size_t> targets_;
  std::vector<std::pair<std::size_t, std::size_t>> branches_;
  std::size_t position_ = 0;
  ir::RegisterId marker_ = 0;
};

}  // namespace
void RewriteEscapes(EscapePlan& plan,
                    const ir::Program& program,
                    std::span<const std::vector<ir::RegisterId>> live,
                    const ir::FlowGraph& graph,
                    std::span<const BlockState> blocks,
                    std::span<const std::optional<std::size_t>> allocations) {
  ScalarRewriter(plan, program, live).Run(graph, blocks, allocations);
}
}  // namespace vela::compiler::native::internal
