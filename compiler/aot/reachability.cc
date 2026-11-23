#include "compiler/aot/reachability.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
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
#include "base/declaration.h"
#include "base/json_decode.h"
#include "base/layout_description.h"
#include "base/member.h"
#include "base/opcode.h"
#include "base/reflection.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"

namespace vela::compiler::aot {
namespace {

enum class NodeKind : std::uint8_t {
  Function,
  Class,
  Type,
  Array,
  Collection,
  Signature,
  Interface,
  Selector,
  JSONPlan,
};

enum class TypeExposure : std::uint8_t { Internal, Host };

struct Node final {
  NodeKind kind;
  std::uint32_t index;
  TypeExposure exposure = TypeExposure::Internal;
  bool operator==(const Node&) const = default;
  template <class H>
  friend H AbslHashValue(H state, const Node& node) {
    return H::combine(std::move(state), node.kind, node.index, node.exposure);
  }
};

class Reachability final {
 public:
  explicit Reachability(const ir::Program& program);
  ~Reachability() = default;
  Reachability(const Reachability&) = delete;
  Reachability& operator=(const Reachability&) = delete;
  Reachability(Reachability&&) = delete;
  Reachability& operator=(Reachability&&) = delete;
  void Mark(NodeKind kind, std::uint32_t index, TypeExposure exposure = TypeExposure::Internal);
  void Trace();
  void Prune(ir::Program& program, std::optional<std::uint32_t>& main) const;

 private:
  void Visit(Node node);
  void VisitFunction(const ir::Function& function, TypeExposure exposure);
  void VisitClass(std::uint32_t index, TypeExposure exposure);
  void VisitType(std::uint32_t index, TypeExposure exposure);
  void VisitLayout(const layout::Type& type, TypeExposure exposure);
  void VisitSource(const frontend::TypeDescriptor& type, TypeExposure exposure);
  void VisitMember(const MemberDefinition& member, TypeExposure exposure);
  void MarkDynamic();
  void PruneClasses(ir::Program& program) const;
  void PruneSelectors(ir::Program& program) const;
  void RemapClasses(ir::Program& program, std::span<const std::uint32_t> mapping) const;
  std::vector<std::uint32_t> PruneJSONPlans(ir::Program& program,
                                            std::span<const std::uint32_t> functions) const;
  template <class T>
  void RemapCallable(T& type, std::uint32_t index, std::span<const std::uint32_t> mapping) const;

  const ir::Program& program_;
  absl::flat_hash_map<std::string_view, Node> names_;
  std::vector<std::vector<std::size_t>> members_;
  std::vector<std::vector<std::uint32_t>> implementations_;
  std::vector<std::vector<std::pair<std::uint32_t, std::uint32_t>>> virtual_implementations_;
  absl::flat_hash_set<Node> reached_;
  std::vector<Node> pending_;
  bool dynamic_ = false;
};

Reachability::Reachability(const ir::Program& program)
    : program_(program),
      members_(program.runtime_types.size()),
      implementations_(program.layouts.interfaces.size()),
      virtual_implementations_(program.selector_declarations.size()) {
  const auto add = [&](const auto& values, NodeKind kind) {
    for (std::size_t index = 0; index < values.size(); ++index) {
      const auto& name = values.at(index).name;
      if (!name.empty()) {
        names_.try_emplace(name, kind, static_cast<std::uint32_t>(index));
      }
    }
  };
  add(program.runtime_types, NodeKind::Type);
  add(program.classes, NodeKind::Class);
  add(program.layouts.arrays, NodeKind::Array);
  add(program.layouts.collections, NodeKind::Collection);
  add(program.layouts.signatures, NodeKind::Signature);
  for (std::size_t index = 0; index < program.classes.size(); ++index) {
    const auto& targets = program.classes[index].dispatch_targets;
    for (std::size_t selector = 0; selector < targets.size(); ++selector) {
      const auto target = targets[selector];
      if (target != kInvalidFunctionTarget) {
        virtual_implementations_[selector].emplace_back(static_cast<std::uint32_t>(index), target);
      }
    }
  }
  for (std::size_t index = 0; index < program.members.size(); ++index) {
    const auto& member = program.members.at(index);
    members_.at(member.receiver_type).push_back(index);
  }
  for (std::size_t index = 0; index < program.classes.size(); ++index) {
    for (const auto interface : program.classes.at(index).interfaces) {
      implementations_.at(interface).push_back(static_cast<std::uint32_t>(index));
    }
  }
}

void Reachability::Mark(NodeKind kind, std::uint32_t index, TypeExposure exposure) {
  if (exposure == TypeExposure::Host) {
    Mark(kind, index);
  }
  const Node node{.kind = kind, .index = index, .exposure = exposure};
  if (reached_.insert(node).second) {
    pending_.push_back(node);
  }
}

void Reachability::MarkDynamic() {
  if (dynamic_) {
    return;
  }
  dynamic_ = true;
  // dynamic 宿主输入可携带任意已描述类型；保留其成员闭包，不能只追踪直接 Call。
  for (std::size_t index = 0; index < program_.runtime_types.size(); ++index) {
    Mark(NodeKind::Type, static_cast<std::uint32_t>(index), TypeExposure::Host);
  }
  for (std::size_t index = 0; index < program_.classes.size(); ++index) {
    Mark(NodeKind::Class, static_cast<std::uint32_t>(index));
  }
}

void Reachability::VisitSource(const frontend::TypeDescriptor& type, TypeExposure exposure) {
  if (type.kind == frontend::TypeDescriptor::Kind::Dynamic) {
    if (exposure == TypeExposure::Host) {
      MarkDynamic();
    }
    return;
  }
  if (const auto found = names_.find(type.name); found != names_.end()) {
    Mark(found->second.kind, found->second.index, exposure);
    return;
  }
  if (type.nullable) {
    const auto name = frontend::NonNullableTypeName(type);
    if (const auto found = names_.find(name); found != names_.end()) {
      Mark(found->second.kind, found->second.index, exposure);
    }
  }
}

void Reachability::VisitFunction(const ir::Function& function, TypeExposure exposure) {
  if (function.json_decode_plan) {
    Mark(NodeKind::JSONPlan, *function.json_decode_plan);
  }
  for (const auto& type : function.value_types) {
    // 局部表示擦除不扩大宿主边界；只有签名、字段和复合载荷传播对外暴露。
    VisitSource(type, TypeExposure::Internal);
  }
  if (!function.native_symbol.empty()) {
    exposure = TypeExposure::Host;
  }
  if (function.signature) {
    const auto& signature = *function.signature;
    if (signature.receiver) {
      Mark(NodeKind::Type, *signature.receiver, exposure);
    }
    for (const auto& parameter : signature.parameters) {
      Mark(NodeKind::Type, parameter.type, exposure);
    }
    if (signature.result) {
      Mark(NodeKind::Type, *signature.result, exposure);
    }
  }
  for (const auto& instruction : function.instructions) {
    switch (instruction.opcode) {
      case Opcode::Call:
        Mark(NodeKind::Function, instruction.index);
        break;
      case Opcode::CallVirtual:
        Mark(NodeKind::Selector, instruction.index);
        break;
      case Opcode::NewInstance:
        Mark(NodeKind::Class, instruction.index);
        break;
      case Opcode::IsType:
      case Opcode::CheckedCast:
        Mark(NodeKind::Type, instruction.index);
        break;
      case Opcode::BuiltinCall:
        switch (static_cast<Builtin>(instruction.index)) {
          case Builtin::DynamicMemberGet:
          case Builtin::DynamicMemberSet:
          case Builtin::DynamicResolveCall:
          case Builtin::DynamicCall:
          case Builtin::DynamicMethodCall:
          case Builtin::DynamicArrayPrepare:
          case Builtin::DynamicArrayInvoke:
          case Builtin::DynamicAsync:
          case Builtin::JsonStringify:
            MarkDynamic();
            break;
          default:
            break;
        }
        break;
      default:
        break;
    }
  }
}

void Reachability::VisitClass(std::uint32_t index, TypeExposure exposure) {
  const auto& source = program_.classes.at(index);
  ir::ForEachClassFunction(source, [&](std::uint32_t target, ir::ClassFunctionKind kind) {
    if (kind != ir::ClassFunctionKind::FieldInitializer &&
        kind != ir::ClassFunctionKind::VirtualEntry &&
        (kind != ir::ClassFunctionKind::Constructor || exposure == TypeExposure::Host)) {
      Mark(NodeKind::Function, target, exposure);
    }
  });
  if (source.callable && source.callable->mode == CallableMode::VirtualMethod) {
    Mark(NodeKind::Selector, source.callable->function, exposure);
  }
  for (std::size_t selector = 0; selector < source.dispatch_targets.size(); ++selector) {
    const auto target = source.dispatch_targets[selector];
    if (target != kInvalidFunctionTarget &&
        reached_.contains(
            {.kind = NodeKind::Selector, .index = static_cast<std::uint32_t>(selector)})) {
      Mark(NodeKind::Function, target, exposure);
    }
  }
  for (const auto& type : source.field_types) {
    VisitSource(type, exposure);
  }
  const auto& layout = program_.layouts.classes.at(index);
  if (layout.callable_type) {
    Mark(NodeKind::Signature, *layout.callable_type, exposure);
  }
  // 类可能只由 NewInstance 引用，而尚未注册为参数约束；动态成员仍按相同名义类型保留。
  if (const auto type = names_.find(source.name);
      type != names_.end() && type->second.kind == NodeKind::Type) {
    Mark(NodeKind::Type, type->second.index, exposure);
  }
}

void Reachability::VisitMember(const MemberDefinition& member, TypeExposure exposure) {
  if (member.kind == MemberKind::Method) {
    Mark(NodeKind::Class, member.target, exposure);
  }
  ir::ForEachPropertyFunction(
      member, [&](std::uint32_t target) { Mark(NodeKind::Function, target, exposure); });
  if (member.type) {
    Mark(NodeKind::Type, *member.type, exposure);
  }
}

void Reachability::VisitType(std::uint32_t index, TypeExposure exposure) {
  VisitLayout(program_.layouts.types.at(index), exposure);
  if (exposure == TypeExposure::Host || dynamic_) {
    for (const auto member : members_.at(index)) {
      VisitMember(program_.members.at(member), exposure);
    }
  }
}

void Reachability::VisitLayout(const layout::Type& type, TypeExposure exposure) {
  using Kind = layout::Type::Kind;
  switch (type.kind) {
    case Kind::Dynamic:
      if (exposure == TypeExposure::Host) {
        MarkDynamic();
      }
      break;
    case Kind::Instance:
    case Kind::Enum:
      Mark(NodeKind::Class, type.index, exposure);
      break;
    case Kind::Interface:
      Mark(NodeKind::Interface, type.index, exposure);
      break;
    case Kind::Array:
      Mark(NodeKind::Array, type.index, exposure);
      break;
    case Kind::Collection:
      Mark(NodeKind::Collection, type.index, exposure);
      break;
    case Kind::Callable:
    case Kind::Future:
    case Kind::Completer:
      Mark(NodeKind::Signature, type.index, exposure);
      break;
    default:
      break;
  }
}

void Reachability::Visit(Node node) {
  const auto& layouts = program_.layouts;
  switch (node.kind) {
    case NodeKind::Function:
      VisitFunction(program_.functions.at(node.index), node.exposure);
      break;
    case NodeKind::Class:
      VisitClass(node.index, node.exposure);
      break;
    case NodeKind::Type:
      VisitType(node.index, node.exposure);
      break;
    case NodeKind::Array:
      VisitLayout(layouts.arrays.at(node.index).element, node.exposure);
      break;
    case NodeKind::Collection: {
      const auto& collection = layouts.collections.at(node.index);
      VisitLayout(collection.key, node.exposure);
      if (collection.value) {
        VisitLayout(*collection.value, node.exposure);
      }
      break;
    }
    case NodeKind::Signature: {
      const auto& signature = layouts.signatures.at(node.index);
      for (const auto& parameter : signature.parameters) {
        VisitLayout(parameter.type, node.exposure);
      }
      if (signature.result) {
        VisitLayout(*signature.result, node.exposure);
      }
      break;
    }
    case NodeKind::Interface:
      for (const auto target : implementations_.at(node.index)) {
        Mark(NodeKind::Class, target, node.exposure);
      }
      break;
    case NodeKind::Selector:
      for (const auto& [owner, target] : virtual_implementations_.at(node.index)) {
        if (reached_.contains({.kind = NodeKind::Class, .index = owner})) {
          const auto host = reached_.contains(
              {.kind = NodeKind::Class, .index = owner, .exposure = TypeExposure::Host});
          Mark(NodeKind::Function, target, host ? TypeExposure::Host : node.exposure);
        }
      }
      break;
    case NodeKind::JSONPlan: {
      const auto& plan = program_.json_decode_plans.at(node.index);
      Mark(NodeKind::Type, plan.type);
      if (plan.constructor != kInvalidFunctionTarget) {
        Mark(NodeKind::Function, plan.constructor);
      }
      if (plan.kind == JSONDecodeKind::Array || plan.kind == JSONDecodeKind::Map ||
          plan.kind == JSONDecodeKind::Nullable) {
        Mark(NodeKind::JSONPlan, plan.element);
      }
      for (const auto& parameter : plan.parameters) {
        Mark(NodeKind::JSONPlan, parameter.node);
      }
      break;
    }
  }
}

void Reachability::Trace() {
  while (!pending_.empty()) {
    const auto node = pending_.back();
    pending_.pop_back();
    // Host 访问覆盖同一节点的 Internal 闭包；内部标记仍供裁剪查询使用。
    if (node.exposure == TypeExposure::Internal &&
        reached_.contains(
            {.kind = node.kind, .index = node.index, .exposure = TypeExposure::Host})) {
      continue;
    }
    Visit(node);
  }
}

template <class T>
void Reachability::RemapCallable(T& type,
                                 std::uint32_t index,
                                 std::span<const std::uint32_t> mapping) const {
  if (!reached_.contains({.kind = NodeKind::Class, .index = index})) {
    type.callable.reset();
    type.callable_type = {};
    return;
  }
  if (type.callable && type.callable->mode != CallableMode::VirtualMethod) {
    const auto target = type.callable->function;
    type.callable->function = mapping[target];
  }
}

std::vector<std::uint32_t> Reachability::PruneJSONPlans(
    ir::Program& program,
    std::span<const std::uint32_t> functions) const {
  std::vector<std::uint32_t> plan_mapping(program.json_decode_plans.size(), kInvalidFunctionTarget);
  std::vector<ir::JSONDecodePlan> plans;
  for (std::size_t index = 0; index < program.json_decode_plans.size(); ++index) {
    if (reached_.contains(
            {.kind = NodeKind::JSONPlan, .index = static_cast<std::uint32_t>(index)})) {
      plan_mapping.at(index) = static_cast<std::uint32_t>(plans.size());
      plans.push_back(std::move(program.json_decode_plans.at(index)));
    }
  }
  program.json_decode_plans = std::move(plans);
  for (auto& plan : program.json_decode_plans) {
    if (plan.constructor != kInvalidFunctionTarget) {
      plan.constructor = functions[plan.constructor];
    }
    if (plan.kind == JSONDecodeKind::Array || plan.kind == JSONDecodeKind::Map ||
        plan.kind == JSONDecodeKind::Nullable) {
      plan.element = plan_mapping.at(plan.element);
    }
    for (auto& parameter : plan.parameters) {
      parameter.node = plan_mapping.at(parameter.node);
    }
  }
  return plan_mapping;
}

void RemapReflectedTargets(ir::Program& program, std::span<const std::uint32_t> mapping) {
  for (auto& member : program.reflection_members) {
    if (member.target != kInvalidFunctionTarget && reflection::IsCallable(member.kind) &&
        (member.flags & reflection::kVirtual) == 0) {
      member.target = mapping[member.target];
    }
  }
}

template <class Visit>
void VisitLayoutTypes(layout::Program& program, Visit visit) {
  for (auto& type : program.types) {
    visit(type);
  }
  for (auto& array : program.arrays) {
    visit(array.element);
  }
  for (auto& collection : program.collections) {
    visit(collection.key);
    if (collection.value) {
      visit(*collection.value);
    }
  }
  for (auto& signature : program.signatures) {
    for (auto& parameter : signature.parameters) {
      visit(parameter.type);
    }
    auto& result = signature.result;
    if (result) {
      visit(*result);
    }
  }
  for (auto& [name, nominal] : program.nominals) {
    for (auto& argument : nominal.arguments) {
      visit(argument);
    }
  }
}

void RemapClassReferences(ir::Program& program, std::span<const std::uint32_t> mapping) {
  VisitLayoutTypes(program.layouts, [&](layout::Type& type) {
    if (type.kind == layout::Type::Kind::Instance || type.kind == layout::Type::Kind::Enum) {
      type.index = mapping[type.index];
    }
  });
  for (auto& function : program.functions) {
    for (auto& instruction : function.instructions) {
      if (instruction.opcode == Opcode::NewInstance) {
        instruction.index = mapping[instruction.index];
      }
    }
  }
  for (auto& member : program.members) {
    if (member.kind == MemberKind::Method) {
      member.target = mapping[member.target];
    }
  }
}

void Reachability::PruneClasses(ir::Program& program) const {
  const auto count = program.classes.size();
  std::vector<std::uint8_t> retained(count);
  std::vector<std::uint32_t> pending;
  const auto mark = [&](std::uint32_t index) {
    auto& selected = retained[index];
    if (selected == 0) {
      selected = 1;
      pending.push_back(index);
    }
  };
  absl::flat_hash_map<DeclarationId, std::uint32_t> declarations;
  for (std::size_t index = 0; index < count; ++index) {
    const auto ordinal = static_cast<std::uint32_t>(index);
    const auto declaration = program.classes[index].declaration;
    if (declaration != DeclarationId{}) {
      declarations.emplace(declaration, ordinal);
    }
    if (reached_.contains({.kind = NodeKind::Class, .index = ordinal})) {
      mark(ordinal);
    }
  }
  // 公共/反射类型与全部结构约束保留原编号。只有既不可创建、也不参与任何类型契约的
  // 实现类才会删除；这些通常是已裁剪函数留下的闭包，不靠类名推断可达性。
  VisitLayoutTypes(program.layouts, [&](const layout::Type& type) {
    if (type.kind == layout::Type::Kind::Instance || type.kind == layout::Type::Kind::Enum) {
      mark(type.index);
    }
  });
  while (!pending.empty()) {
    const auto index = pending.back();
    pending.pop_back();
    const auto base = program.classes[index].base_class;
    if (base != DeclarationId{}) {
      mark(declarations.at(base));
    }
  }
  std::vector<std::uint32_t> mapping(count, kInvalidFunctionTarget);
  std::vector<ir::Class> classes;
  std::vector<layout::Class> layouts;
  for (std::size_t index = 0; index < count; ++index) {
    if (retained[index] != 0) {
      mapping[index] = static_cast<std::uint32_t>(classes.size());
      classes.push_back(std::move(program.classes[index]));
      layouts.push_back(std::move(program.layouts.classes[index]));
    }
  }
  program.classes = std::move(classes);
  program.layouts.classes = std::move(layouts);
  RemapClassReferences(program, mapping);
}

void Reachability::RemapClasses(ir::Program& program,
                                std::span<const std::uint32_t> mapping) const {
  for (std::size_t index = 0; index < program.classes.size(); ++index) {
    auto& type = program.classes.at(index);
    if (type.constructor != kInvalidFunctionTarget) {
      type.constructor = mapping[type.constructor];
    }
    const auto class_index = static_cast<std::uint32_t>(index);
    RemapCallable(type, class_index, mapping);
    if (!reached_.contains({.kind = NodeKind::Class, .index = class_index})) {
      std::ranges::fill(type.dispatch_targets, kInvalidFunctionTarget);
    } else {
      for (auto& target : type.dispatch_targets) {
        if (target != kInvalidFunctionTarget) {
          target = mapping[target];
        }
      }
    }
    RemapCallable(program.layouts.classes.at(index), class_index, mapping);
  }
}

void RemapSelectorReferences(ir::Program& program, std::span<const std::uint32_t> mapping) {
  const auto remap_callable = [&](auto& type) {
    auto& callable = type.callable;
    if (callable && callable->mode == CallableMode::VirtualMethod) {
      callable->function = mapping[callable->function];
    }
  };
  for (auto& type : program.classes) {
    auto& targets = type.dispatch_targets;
    std::vector<std::uint32_t> compact(program.selector_declarations.size(),
                                       kInvalidFunctionTarget);
    for (std::size_t index = 0; index < targets.size(); ++index) {
      const auto selector = mapping[index];
      if (selector != kInvalidFunctionTarget) {
        compact[selector] = targets[index];
      }
    }
    targets = std::move(compact);
    remap_callable(type);
  }
  for (auto& type : program.layouts.classes) {
    remap_callable(type);
  }
  for (auto& function : program.functions) {
    for (auto& instruction : function.instructions) {
      if (instruction.opcode == Opcode::CallVirtual) {
        instruction.index = mapping[instruction.index];
      }
    }
  }
  for (auto& member : program.reflection_members) {
    if ((member.flags & reflection::kVirtual) != 0 && member.target != kInvalidFunctionTarget) {
      member.target = mapping[member.target];
    }
  }
}

void Reachability::PruneSelectors(ir::Program& program) const {
  const auto count = program.selector_declarations.size();
  std::vector<std::uint32_t> mapping(count, kInvalidFunctionTarget);
  std::vector<std::uint8_t> reflected(count);
  for (const auto& member : program.reflection_members) {
    if ((member.flags & reflection::kVirtual) != 0 && member.target != kInvalidFunctionTarget) {
      reflected[member.target] = 1;
    }
  }
  std::vector<DeclarationId> declarations;
  for (std::size_t index = 0; index < count; ++index) {
    const Node selector{.kind = NodeKind::Selector, .index = static_cast<std::uint32_t>(index)};
    if (reflected[index] != 0 || reached_.contains(selector)) {
      mapping[index] = static_cast<std::uint32_t>(declarations.size());
      declarations.push_back(program.selector_declarations[index]);
    }
  }
  program.selector_declarations = std::move(declarations);
  RemapSelectorReferences(program, mapping);
}

void Reachability::Prune(ir::Program& program, std::optional<std::uint32_t>& main) const {
  std::vector<std::uint32_t> mapping(program.functions.size(), kInvalidFunctionTarget);
  std::vector<ir::Function> functions;
  for (std::size_t index = 0; index < mapping.size(); ++index) {
    if (reached_.contains(
            {.kind = NodeKind::Function, .index = static_cast<std::uint32_t>(index)})) {
      mapping.at(index) = static_cast<std::uint32_t>(functions.size());
      functions.push_back(std::move(program.functions.at(index)));
    }
  }
  program.functions = std::move(functions);
  const auto plan_mapping = PruneJSONPlans(program, mapping);
  for (auto& function : program.functions) {
    if (function.json_decode_plan) {
      function.json_decode_plan = plan_mapping.at(*function.json_decode_plan);
      // 标准强类型入口的第一个常量是传给运行时的计划编号。
      function.instructions.front().immediate = *function.json_decode_plan;
    }
    for (auto& instruction : function.instructions) {
      if (instruction.opcode == Opcode::Call) {
        instruction.index = mapping.at(instruction.index);
      }
    }
  }
  for (auto& initializer : program.global_initializers) {
    initializer = mapping.at(initializer);
  }
  if (main) {
    main = mapping.at(*main);
  }
  RemapClasses(program, mapping);
  std::erase_if(program.members, [&](const MemberDefinition& member) {
    return !reached_.contains(
        {.kind = NodeKind::Type, .index = member.receiver_type, .exposure = TypeExposure::Host});
  });
  for (auto& member : program.members) {
    if (member.kind == MemberKind::Property && member.target != kInvalidFunctionTarget) {
      member.target = mapping.at(member.target);
    }
    if (member.setter) {
      member.setter = mapping.at(*member.setter);
    }
  }
  RemapReflectedTargets(program, mapping);
  PruneClasses(program);
  // 封闭程序的 selector 序号只在本映像内使用。保留声明身份，并同步虚调用、方法值和反射，
  // 避免已裁剪方法继续在每个类的分派表中占据空槽。
  PruneSelectors(program);
}

}  // namespace

void PruneNativeProgram(ir::Program& program, std::optional<std::uint32_t>& main) {
  Reachability reachability(program);
  for (std::size_t index = 0; index < program.functions.size(); ++index) {
    if (program.functions[index].is_exported) {
      reachability.Mark(NodeKind::Function, static_cast<std::uint32_t>(index), TypeExposure::Host);
    }
  }
  for (const auto initializer : program.global_initializers) {
    reachability.Mark(NodeKind::Function, initializer);
  }
  if (main) {
    reachability.Mark(NodeKind::Function, *main);
  }
  for (const auto& member : program.reflection_members) {
    if ((member.flags & reflection::kRegistered) == 0 ||
        (member.flags & reflection::kPrivate) != 0 || member.target == kInvalidFunctionTarget) {
      continue;
    }
    if (reflection::IsCallable(member.kind)) {
      const auto& owner = program.layouts.types.at(member.declaring_type);
      if ((member.flags & reflection::kVirtual) != 0) {
        reachability.Mark(NodeKind::Selector, member.target);
      }
      if (owner.kind == layout::Type::Kind::Interface) {
        reachability.Mark(NodeKind::Interface, owner.index);
      } else {
        reachability.Mark(NodeKind::Class, owner.index);
        if ((member.flags & reflection::kVirtual) == 0) {
          reachability.Mark(NodeKind::Function, member.target);
        }
      }
    }
  }
  reachability.Trace();
  reachability.Prune(program, main);
}

}  // namespace vela::compiler::aot
