#include "compiler/aot/native_reflection_optimizer.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "base/builtin.h"
#include "base/callable.h"
#include "base/native_program_mode.h"
#include "base/opcode.h"
#include "base/reflection.h"
#include "base/reflection_operation.h"
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"
#include "compiler/native/code_generator.h"
#include "compiler/native/global_analysis.h"

namespace vela::compiler::aot {
namespace {

enum class FactKind : std::uint8_t { Unknown, Null, Integer, String, Type, Member };

struct Fact final {
  FactKind kind = FactKind::Unknown;
  std::int64_t index = 0;
  std::string_view text;

  bool operator==(const Fact&) const = default;
};

using MemberKey = std::tuple<std::uint32_t, reflection::MemberKind, std::string_view>;
using TypeKey = std::pair<std::string_view, std::string_view>;

class ReflectionBindings final {
 public:
  ReflectionBindings(ir::Program& program, std::span<const std::uint8_t> readonly)
      : program_(&program),
        readonly_(readonly),
        summaries_(program.functions.size()),
        values_(program.functions.size()) {
    for (const auto& type : program.reflection_types) {
      types_.emplace(type.type, &type);
      if ((type.flags & reflection::kRegistered) != 0 && type.arguments.empty()) {
        const auto [entry, inserted] =
            named_types_.try_emplace(TypeKey{type.module, type.name},
                                     Fact{.kind = FactKind::Type, .index = type.type, .text = {}});
        if (!inserted) {
          entry->second = {};
        }
      }
    }
    for (std::size_t index = 0; index < program.reflection_members.size(); ++index) {
      const auto& member = program.reflection_members[index];
      if ((member.flags & (reflection::kRegistered | reflection::kPrivate | reflection::kStatic)) !=
              reflection::kRegistered ||
          !member.type_arguments.empty()) {
        continue;
      }
      const auto [entry, inserted] = members_.try_emplace(
          MemberKey{member.declaring_type, member.kind, member.name},
          Fact{.kind = FactKind::Member, .index = static_cast<std::int64_t>(index), .text = {}});
      if (!inserted) {
        entry->second = {};
      }
    }
  }

  void Bind() {
    auto& functions = program_->functions;
    std::vector<std::vector<std::size_t>> callers(functions.size());
    std::vector<std::size_t> pending;
    std::vector<std::uint8_t> queued(functions.size(), 1);
    for (std::size_t index = 0; index < functions.size(); ++index) {
      pending.push_back(index);
      for (const auto& instruction : functions[index].instructions) {
        if (instruction.opcode == Opcode::GetGlobal && readonly_[instruction.index] != 0) {
          callers[program_->global_initializers[instruction.index]].push_back(index);
        } else if (instruction.opcode == Opcode::Call &&
                   functions[instruction.index].parameter_count == 0) {
          callers[instruction.index].push_back(index);
        }
      }
    }
    // 摘要只从未知收敛到同一返回来源；递归环没有凭空产生描述符的基例。
    while (!pending.empty()) {
      const auto index = pending.back();
      pending.pop_back();
      queued[index] = 0;
      values_[index] = Values(functions[index]);
      const auto summary = Summary(functions[index], values_[index]);
      if (summary == summaries_[index]) {
        continue;
      }
      summaries_[index] = summary;
      for (const auto caller : callers[index]) {
        if (queued[caller] == 0) {
          queued[caller] = 1;
          pending.push_back(caller);
        }
      }
    }
    for (std::size_t index = 0; index < functions.size(); ++index) {
      BindFunction(functions[index], values_[index]);
    }
  }

 private:
  static void BindFunction(ir::Function& function, std::span<const Fact> values) {
    for (auto& instruction : function.instructions) {
      if (!ir::IsBuiltinCall(instruction, Builtin::Reflection) &&
          !ir::IsBuiltinCall(instruction, Builtin::ReflectionInvoke)) {
        continue;
      }
      const auto member = values[instruction.lhs + 1];
      if (member.kind == FactKind::Member) {
        instruction.reflection_member = static_cast<std::uint32_t>(member.index);
      }
    }
  }

  [[nodiscard]] Fact FindMember(std::span<const Fact> values,
                                const ir::Instruction& instruction,
                                reflection::MemberKind kind) const {
    const auto base = instruction.lhs;
    const auto type = values[base + 1];
    const auto name = values[base + 2];
    if (type.kind != FactKind::Type || name.kind != FactKind::String ||
        values[base + 3].kind != FactKind::Null ||
        (kind == reflection::MemberKind::Method && values[base + 4].kind != FactKind::Null)) {
      return {};
    }
    const auto owner = types_.find(static_cast<std::uint32_t>(type.index));
    // 不复制继承查询的覆盖排序；有基类的默认查询继续由共享索引解析。
    if (owner == types_.end() || owner->second->base != kInvalidFunctionTarget ||
        (owner->second->flags & reflection::kRegistered) == 0) {
      return {};
    }
    const auto found = members_.find({static_cast<std::uint32_t>(type.index), kind, name.text});
    return found == members_.end() ? Fact{} : found->second;
  }

  [[nodiscard]] Fact ReflectionResult(std::span<const Fact> values,
                                      const ir::Instruction& instruction) const {
    const auto base = instruction.lhs;
    const auto operation = values[base];
    if (operation.kind != FactKind::Integer) {
      return {};
    }
    switch (static_cast<reflection::Operation>(operation.index)) {
      case reflection::Operation::Module: {
        const auto type = values[base + 1];
        if (type.kind == FactKind::Type) {
          const auto found = types_.find(static_cast<std::uint32_t>(type.index));
          if (found != types_.end()) {
            return {.kind = FactKind::String, .text = found->second->module};
          }
        }
        return {};
      }
      case reflection::Operation::FindType: {
        const auto module = values[base + 1];
        const auto name = values[base + 2];
        if (module.kind != FactKind::String || name.kind != FactKind::String ||
            values[base + 3].kind != FactKind::Null) {
          return {};
        }
        const auto found = named_types_.find({module.text, name.text});
        return found == named_types_.end() ? Fact{} : found->second;
      }
      case reflection::Operation::FindField:
        return FindMember(values, instruction, reflection::MemberKind::Field);
      case reflection::Operation::FindMethod:
        return FindMember(values, instruction, reflection::MemberKind::Method);
      default:
        return {};
    }
  }

  [[nodiscard]] Fact Result(std::span<const Fact> values,
                            const ir::Instruction& instruction) const {
    switch (instruction.opcode) {
      case Opcode::LoadNull:
        return {.kind = FactKind::Null, .text = {}};
      case Opcode::LoadInt:
        return {.kind = FactKind::Integer, .index = instruction.immediate, .text = {}};
      case Opcode::LoadString:
        return {.kind = FactKind::String, .text = program_->strings[instruction.index]};
      case Opcode::GetGlobal:
        return readonly_[instruction.index] != 0
                   ? summaries_[program_->global_initializers[instruction.index]]
                   : Fact{};
      case Opcode::Call:
        return program_->functions[instruction.index].parameter_count == 0
                   ? summaries_[instruction.index]
                   : Fact{};
      case Opcode::BuiltinCall:
        if (ir::IsBuiltinCall(instruction, Builtin::ReflectionTypeInfo)) {
          const auto type = values[instruction.lhs];
          return type.kind == FactKind::Integer
                     ? Fact{.kind = FactKind::Type, .index = type.index, .text = {}}
                     : Fact{};
        }
        if (ir::IsBuiltinCall(instruction, Builtin::Reflection)) {
          return ReflectionResult(values, instruction);
        }
        return {};
      default:
        return {};
    }
  }

  [[nodiscard]] std::vector<Fact> Values(const ir::Function& function) const {
    const auto accesses = ir::AnalyzeRegisterAccesses(function, program_->functions);
    const auto flow = ir::AnalyzeRegisterFlow(function, accesses);
    std::vector<Fact> values(function.register_count);
    const auto read = [&](ir::RegisterId reg) {
      const auto origin = flow.origins[reg];
      return origin ? values[*origin] : Fact{};
    };
    // 只让单定义值及其 Move 别名传播事实；循环覆写和合流不建立猜测。
    for (std::size_t index = 0; index < function.instructions.size(); ++index) {
      const auto& instruction = function.instructions[index];
      if (!accesses[index].defines_value || flow.definitions[instruction.dst].count != 1) {
        continue;
      }
      auto result =
          instruction.opcode == Opcode::Move ? read(instruction.lhs) : Result(values, instruction);
      values[instruction.dst] = result;
    }
    return values;
  }

  static Fact Summary(const ir::Function& function, std::span<const Fact> values) {
    std::optional<Fact> result;
    for (const auto& instruction : function.instructions) {
      if (instruction.opcode != Opcode::Return) {
        continue;
      }
      const auto value = values[instruction.dst];
      if (result && value != *result) {
        return {};
      }
      result = value;
    }
    return result.value_or(Fact{});
  }

  ir::Program* program_;
  std::span<const std::uint8_t> readonly_;
  std::vector<Fact> summaries_;
  std::vector<std::vector<Fact>> values_;
  absl::flat_hash_map<std::uint32_t, const reflection::Type*> types_;
  absl::flat_hash_map<TypeKey, Fact> named_types_;
  absl::flat_hash_map<MemberKey, Fact> members_;
};

}  // namespace

void BindNativeReflectedMembers(ir::Program& program) {
  if (program.native_mode != NativeProgramMode::Closed || program.reflection_members.empty()) {
    return;
  }
  const auto globals = native::AnalyzeGlobalAccess(program, NativeCodePlacement::Linked);
  ReflectionBindings(program, globals.readonly_slots).Bind();
}

}  // namespace vela::compiler::aot
