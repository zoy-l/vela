#include "compiler/ir/lowerer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/array_algorithm.h"
#include "base/builtin.h"
#include "base/callable.h"
#include "base/member.h"
#include "compiler/frontend/ast.h"
#include "compiler/frontend/builtins.h"
#include "compiler/frontend/type_checker.h"
#include "compiler/ir/ir.h"

namespace vela::ir {

void Lowerer::RegisterArrayAlgorithmMember(const frontend::TypeDescriptor& receiver,
                                           std::string_view name,
                                           ArrayAlgorithm algorithm) {
  using Kind = frontend::TypeDescriptor::Kind;
  const frontend::TypeDescriptor dynamic{.kind = Kind::Dynamic, .name = "dynamic"};
  const auto& element = model_.array_elements.at(receiver.name);
  auto callback_signature = frontend::ArrayCallbackSignature(algorithm, element, dynamic);
  callback_signature.return_type = dynamic;
  auto callback =
      CreateMemberFunction(receiver, std::string(name) + ".callback", callback_signature);
  const auto callback_target = function_ids_.at(callback.name);
  current_function_ = &callback;
  current_handler_.reset();
  next_register_ = callback.parameter_count;
  const auto context = EmitTemporary({.opcode = Opcode::GetField, .lhs = 0, .location = {}});
  const auto empty = EmitTemporary({.opcode = Opcode::LoadNull, .location = {}});
  std::array<RegisterId, kDynamicOperationArity> arguments{context, empty, empty, empty, empty};
  for (std::size_t index = 0; index < callback_signature.parameters.size(); ++index) {
    arguments.at(index + 1) = static_cast<RegisterId>(index + 1);
  }
  const auto invoked = AllocateTemporary({});
  EmitBuiltinCall(Builtin::DynamicArrayInvoke, invoked, arguments, {});
  Emit({.opcode = Opcode::Return, .dst = invoked, .location = {}});
  callback.register_count = next_register_;
  StoreFunction(std::move(callback));
  current_function_ = nullptr;
  const std::vector<frontend::Capture> captures{
      {.name = "plan", .type = dynamic, .binding = 0, .kind = frontend::BindingKind::Constant},
  };
  const auto closure = CallableClass(callback_target, CallableMode::Closure, captures,
                                     frontend::MakeFunctionType(callback_signature));

  auto signature = frontend::ArrayAlgorithmSignature(algorithm, element, dynamic);
  signature.parameters.front().type = dynamic;
  if (algorithm == ArrayAlgorithm::Map) {
    signature.return_type = dynamic;
  }
  auto function = CreateMemberFunction(receiver, name, signature);
  current_function_ = &function;
  current_handler_.reset();
  next_register_ = function.parameter_count;
  const auto initial = algorithm == ArrayAlgorithm::Reduce
                           ? RegisterId{2}
                           : EmitTemporary({.opcode = Opcode::LoadNull, .location = {}});
  const auto selector = EmitTemporary({
      .opcode = Opcode::LoadInt,
      .immediate = static_cast<std::int64_t>(algorithm),
      .location = {},
  });
  const auto wrapper =
      EmitTemporary({.opcode = Opcode::LoadInt, .immediate = closure, .location = {}});
  const auto plan = AllocateTemporary({});
  const std::array prepare{RegisterId{0}, RegisterId{1}, initial, selector, wrapper};
  EmitBuiltinCall(Builtin::DynamicArrayPrepare, plan, prepare, {});
  const std::array plan_argument{plan};
  EmitBuiltinCall(Builtin::DynamicArrayCallback, 1, plan_argument, {});
  const auto length = EmitTemporary({
      .opcode = Opcode::BuiltinCall,
      .lhs = 0,
      .index = static_cast<std::uint32_t>(Builtin::ArrayLength),
      .location = {},
  });
  const auto guard = AllocateRegister({});
  static_cast<void>(AllocateRegister({}));
  Emit({.opcode = Opcode::Move, .dst = guard, .lhs = 0, .location = {}});
  Emit({
      .opcode = Opcode::BuiltinCall,
      .dst = guard + 1,
      .lhs = 0,
      .index = static_cast<std::uint32_t>(Builtin::ArrayMutation),
      .location = {},
  });
  std::optional<RegisterId> output;
  if (algorithm == ArrayAlgorithm::Map) {
    output = AllocateTemporary({});
    EmitBuiltinCall(Builtin::DynamicArrayOutput, *output, plan_argument, {});
  }
  const auto result = LowerArrayTraversal(
      {
          .array = 0,
          .callback = 1,
          .length = length,
          .guard = guard,
          .positions = 0,
          .callback_arity = callback_signature.parameters.size(),
          .comparison = ArrayAlgorithmRegisters::Comparison::Integer,
          .element_type = element,
          .signature = &callback_signature,
          .validation_plan = plan,
      },
      algorithm, receiver, signature.return_type, {}, output);
  Emit({.opcode = Opcode::Return, .dst = result.reg, .location = {}});
  function.register_count = next_register_;
  PublishMemberFunction(receiver, name, MemberKind::Method, signature, std::move(function));
}

}  // namespace vela::ir
