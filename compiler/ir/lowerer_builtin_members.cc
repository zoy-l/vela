#include "compiler/ir/lowerer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include "base/builtin.h"
#include "base/callable.h"
#include "base/member.h"
#include "base/regexp.h"
#include "compiler/frontend/ast.h"
#include "compiler/frontend/builtins.h"
#include "compiler/frontend/type_checker.h"
#include "compiler/ir/ir.h"

namespace vela::ir {

void Lowerer::RegisterBuiltinMembers() {
  using Kind = frontend::TypeDescriptor::Kind;
  absl::flat_hash_set<std::string> requested;
  for (const auto& [expression, type] : model_.expression_types) {
    static_cast<void>(type);
    if (expression->kind != frontend::Expression::Kind::Member) {
      continue;
    }
    const auto object = model_.expression_types.find(expression->object.get());
    if (object != model_.expression_types.end() && object->second.kind == Kind::Dynamic) {
      requested.insert(expression->text);
    }
  }
  std::vector<std::string> names(requested.begin(), requested.end());
  std::ranges::sort(names);
  std::vector<frontend::TypeDescriptor> receivers;
  for (const auto& [name, type] : model_.types) {
    if (name == type.name && !type.nullable && type.kind != Kind::Invalid &&
        type.kind != Kind::Null && type.kind != Kind::Void && type.kind != Kind::Dynamic &&
        type.kind != Kind::Class && type.kind != Kind::Interface && type.kind != Kind::Enum &&
        type.kind != Kind::Function) {
      receivers.push_back(type);
    }
  }
  std::ranges::sort(receivers, {}, &frontend::TypeDescriptor::name);
  absl::flat_hash_map<Kind, std::vector<std::string>> members_by_kind;
  for (const auto& receiver : receivers) {
    const auto known = members_by_kind.find(receiver.kind);
    if (known != members_by_kind.end()) {
      RegisterBuiltinReceiver(receiver, known->second);
      continue;
    }
    const auto first = program_.members.size();
    RegisterBuiltinReceiver(receiver, names);
    auto& supported = members_by_kind.try_emplace(receiver.kind).first->second;
    // 同种标准类型的泛型实例共享成员名称；任意业务成员名只解析一次。
    for (const auto& member : std::span(program_.members).subspan(first)) {
      supported.push_back(member.name);
    }
  }
}

void Lowerer::RegisterBuiltinReceiver(const frontend::TypeDescriptor& receiver,
                                      std::span<const std::string> names) {
  for (const auto& name : names) {
    RegisterBuiltinReceiverMember(receiver, name);
  }
}

void Lowerer::RegisterBuiltinReceiverMember(const frontend::TypeDescriptor& receiver,
                                            std::string_view name) {
  using Kind = frontend::TypeDescriptor::Kind;
  if (receiver.kind == Kind::Future || receiver.kind == Kind::Completer ||
      receiver.kind == Kind::Timer || receiver.kind == Kind::CancellationToken) {
    RegisterAsyncMember(receiver, name);
    return;
  }
  if (const auto property = frontend::FindBuiltinProperty(receiver.kind, name)) {
    RegisterBuiltinMember(receiver, name, *property, frontend::BuiltinSignature(*property),
                          MemberKind::Property);
  } else if (const auto builtin = frontend::FindBuiltin(receiver.kind, name)) {
    const auto& signature = frontend::BuiltinSignature(*builtin);
    if (std::ranges::any_of(signature.parameters, [this](const auto& parameter) {
          return parameter.type.kind == Kind::Enum && !model_.types.contains(parameter.type.name);
        })) {
      return;
    }
    RegisterBuiltinMember(receiver, name, *builtin, signature, MemberKind::Method);
  } else if (receiver.kind == Kind::Array) {
    if (const auto algorithm = frontend::FindArrayAlgorithm(name)) {
      RegisterArrayAlgorithmMember(receiver, name, *algorithm);
      return;
    }
    const auto methods = frontend::ArrayMethods();
    const auto method =
        std::ranges::find(methods, name, [](const auto& item) { return item.first; });
    if (method != methods.end()) {
      RegisterBuiltinMember(
          receiver, name, method->second,
          frontend::ArrayMethodSignature(method->second, model_.array_elements.at(receiver.name)),
          MemberKind::Method);
    } else if (name == "join") {
      RegisterBuiltinMember(receiver, name, Builtin::ArrayJoin,
                            frontend::BuiltinSignature(Builtin::ArrayJoin), MemberKind::Method);
    }
  } else if (receiver.kind == Kind::Set || receiver.kind == Kind::Map) {
    const auto method = frontend::ResolveCollectionBuiltin(
        receiver, model_.generic_arguments.at(receiver.name), name);
    if (method) {
      RegisterBuiltinMember(receiver, name, method->operation, method->signature,
                            MemberKind::Method);
    }
  }
}

Function Lowerer::CreateMemberFunction(const frontend::TypeDescriptor& receiver,
                                       std::string_view name,
                                       const frontend::FunctionDescriptor& signature) {
  using Kind = frontend::TypeDescriptor::Kind;
  Function function;
  function.name = "$dynamic." + receiver.name + "." + std::string(name);
  static_cast<void>(AddFunctionId(function.name));
  program_.functions.resize(persistent_ == nullptr ? function_ids_.size()
                                                   : persistent_->functions.size());
  function.return_type = signature.return_type;
  function.parameter_count = static_cast<std::uint32_t>(signature.parameters.size() + 1);
  function.value_types.push_back(receiver);
  function.signature.emplace();
  auto& call = *function.signature;
  call.hidden_parameters = 1;
  for (const auto& parameter : signature.parameters) {
    const auto type = parameter.rest ? receiver : parameter.type;
    call.parameters.push_back({
        .name = parameter.name,
        .type = RegisterRuntimeType(type),
        .optional = signature.optional_parameters.contains(parameter.name),
        .rest = parameter.rest,
    });
  }
  if (function.return_type.kind != Kind::Void) {
    call.result = RegisterRuntimeType(function.return_type);
  }
  return function;
}

void Lowerer::RegisterBuiltinMember(const frontend::TypeDescriptor& receiver,
                                    std::string_view name,
                                    Builtin builtin,
                                    const frontend::FunctionDescriptor& signature,
                                    MemberKind kind) {
  auto function = CreateMemberFunction(receiver, name, signature);
  current_function_ = &function;
  current_handler_.reset();
  next_register_ = function.parameter_count;
  const auto arity = BuiltinArity(builtin);
  const auto base = AllocateRegisterWindow(arity, {});
  Emit({.opcode = Opcode::Move, .dst = base, .lhs = 0, .location = {}});
  frontend::Expression expression;
  LowerBuiltinDefaults(builtin, expression, base);
  if (builtin == Builtin::ArraySplice) {
    Emit({.opcode = Opcode::LoadInt, .dst = base + 1, .location = {}});
    Emit({
        .opcode = Opcode::LoadInt,
        .dst = base + 2,
        .immediate = std::numeric_limits<std::int64_t>::max(),
        .location = {},
    });
  }
  for (std::size_t index = 0; index < signature.parameters.size(); ++index) {
    const auto& parameter = signature.parameters.at(index);
    const auto source = static_cast<RegisterId>(index + 1);
    const auto skip = function.instructions.size();
    if (signature.optional_parameters.contains(parameter.name)) {
      Emit({.opcode = Opcode::JumpIfNull, .lhs = source, .location = {}});
    }
    Emit({
        .opcode = Opcode::Move,
        .dst = static_cast<RegisterId>(base + source),
        .lhs = source,
        .location = {},
    });
    if (signature.optional_parameters.contains(parameter.name)) {
      function.instructions.at(skip).index =
          static_cast<std::uint32_t>(function.instructions.size());
    }
  }
  if (builtin == Builtin::ArrayPush) {
    builtin = Builtin::ArrayPushMany;
  }
  if (builtin == Builtin::StringCodePoints || builtin == Builtin::StringGraphemes ||
      builtin == Builtin::CollectionKeys || builtin == Builtin::CollectionValues) {
    auto count_builtin = Builtin::CollectionSize;
    if (builtin == Builtin::StringCodePoints) {
      count_builtin = Builtin::StringLength;
    } else if (builtin == Builtin::StringGraphemes) {
      count_builtin = Builtin::StringGraphemeLength;
    }
    const auto count = EmitTemporary({
        .opcode = Opcode::BuiltinCall,
        .lhs = base,
        .index = static_cast<std::uint32_t>(count_builtin),
        .location = {},
    });
    Emit({
        .opcode = Opcode::NewDynamicArray,
        .dst = base + 1,
        .lhs = count,
        .index = RegisterArrayType(function.return_type),
        .location = {},
    });
  }
  if (HasRegExpArrayLayout(builtin)) {
    auto type = function.return_type;
    if (type.nullable) {
      type.nullable = false;
      type.name.pop_back();
    }
    Emit({
        .opcode = Opcode::LoadInt,
        .dst = static_cast<RegisterId>(base + arity - 1),
        .immediate = RegisterArrayType(type),
        .location = {},
    });
  }
  const auto result = EmitTemporary({
      .opcode = Opcode::BuiltinCall,
      .lhs = base,
      .index = static_cast<std::uint32_t>(builtin),
      .location = {},
  });
  Emit({.opcode = Opcode::Return, .dst = result, .location = {}});
  function.register_count = next_register_;
  PublishMemberFunction(receiver, name, kind, signature, std::move(function));
}

void Lowerer::PublishMemberFunction(const frontend::TypeDescriptor& receiver,
                                    std::string_view name,
                                    MemberKind kind,
                                    const frontend::FunctionDescriptor& signature,
                                    Function function) {
  const auto target = function_ids_.at(function.name);
  StoreFunction(std::move(function));
  current_function_ = nullptr;
  const auto receiver_type = RegisterRuntimeType(receiver);
  auto member_target = target;
  if (kind == MemberKind::Method) {
    const std::vector<frontend::Capture> captures{
        {.name = "this", .type = receiver, .binding = 0, .kind = frontend::BindingKind::Constant},
    };
    member_target = CallableClass(target, CallableMode::BoundMethod, captures,
                                  frontend::MakeFunctionType(signature));
  }
  program_.members.push_back({
      .receiver_type = receiver_type,
      .name = std::string(name),
      .kind = kind,
      .target = member_target,
  });
}

}  // namespace vela::ir
