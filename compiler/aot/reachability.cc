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
#include "base/layout_description.h"
#include "base/member.h"
#include "base/opcode.h"
#include "compiler/frontend/type_checker.h"
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
  template <class T>
  void RemapClass(T& type, std::uint32_t index, std::span<const std::uint32_t> mapping) const;

  const ir::Program& program_;
  absl::flat_hash_map<std::string, Node> names_;
  std::vector<std::vector<std::size_t>> members_;
  std::vector<std::vector<std::uint32_t>> implementations_;
  absl::flat_hash_set<Node> reached_;
  std::vector<Node> pending_;
  bool dynamic_ = false;
};

Reachability::Reachability(const ir::Program& program)
    : program_(program),
      members_(program.runtime_types.size()),
      implementations_(program.layouts.interfaces.size()) {
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
    Mark(NodeKind::Type, static_cast<std::uint32_t>(index));
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
    const auto name = type.kind == frontend::TypeDescriptor::Kind::Function
                          ? std::string_view(type.name).substr(1, type.name.size() - 3)
                          : std::string_view(type.name).substr(0, type.name.size() - 1);
    if (const auto found = names_.find(name); found != names_.end()) {
      Mark(found->second.kind, found->second.index, exposure);
    }
  }
}

void Reachability::VisitFunction(const ir::Function& function, TypeExposure exposure) {
  for (const auto& type : function.value_types) {
    // 局部表示擦除不扩大宿主边界；只有签名、字段和复合载荷传播对外暴露。
    VisitSource(type, TypeExposure::Internal);
  }
  if (!function.native_symbol.empty()) {
    exposure = TypeExposure::Host;
  }
  if (function.signature) {
    const auto& signature = *function.signature;
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
      case Opcode::NewInstance:
      case Opcode::NewOk:
      case Opcode::NewErr:
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
  if (source.callable && source.callable->mode != CallableMode::InterfaceMethod) {
    Mark(NodeKind::Function, source.callable->function, exposure);
  }
  for (const auto target : source.dispatch_targets) {
    if (target != kInvalidFunctionTarget) {
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
  if (layout.expected_type) {
    Mark(NodeKind::Signature, *layout.expected_type, exposure);
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
  } else if (member.kind == MemberKind::Property && member.target != kInvalidFunctionTarget) {
    Mark(NodeKind::Function, member.target, exposure);
  }
  if (member.setter) {
    Mark(NodeKind::Function, *member.setter, exposure);
  }
  if (member.type) {
    Mark(NodeKind::Type, *member.type, exposure);
  }
}

void Reachability::VisitType(std::uint32_t index, TypeExposure exposure) {
  VisitLayout(program_.layouts.types.at(index), exposure);
  for (const auto member : members_.at(index)) {
    VisitMember(program_.members.at(member), exposure);
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
    case Kind::Expected:
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
  }
}

void Reachability::Trace() {
  while (!pending_.empty()) {
    const auto node = pending_.back();
    pending_.pop_back();
    Visit(node);
  }
}

template <class T>
void Reachability::RemapClass(T& type,
                              std::uint32_t index,
                              std::span<const std::uint32_t> mapping) const {
  if (!reached_.contains({.kind = NodeKind::Class, .index = index})) {
    type.callable.reset();
    std::ranges::fill(type.dispatch_targets, kInvalidFunctionTarget);
    return;
  }
  if (type.callable && type.callable->mode != CallableMode::InterfaceMethod) {
    const auto target = type.callable->function;
    type.callable->function = mapping.subspan(target).front();
  }
  for (auto& target : type.dispatch_targets) {
    if (target != kInvalidFunctionTarget) {
      target = mapping.subspan(target).front();
    }
  }
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
  for (auto& function : program.functions) {
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
  for (std::size_t index = 0; index < program.classes.size(); ++index) {
    RemapClass(program.classes.at(index), static_cast<std::uint32_t>(index), mapping);
    RemapClass(program.layouts.classes.at(index), static_cast<std::uint32_t>(index), mapping);
  }
  std::erase_if(program.members, [&](const MemberDefinition& member) {
    return !reached_.contains({.kind = NodeKind::Type, .index = member.receiver_type});
  });
  for (auto& member : program.members) {
    if (member.kind == MemberKind::Property && member.target != kInvalidFunctionTarget) {
      member.target = mapping.at(member.target);
    }
    if (member.setter) {
      member.setter = mapping.at(*member.setter);
    }
  }
}

}  // namespace

void PruneNativeProgram(ir::Program& program,
                        std::span<const std::string> exports,
                        std::optional<std::uint32_t>& main) {
  Reachability reachability(program);
  const absl::flat_hash_set<std::string_view> selected(exports.begin(), exports.end());
  for (std::size_t index = 0; index < program.functions.size(); ++index) {
    if (selected.contains(program.functions.at(index).name)) {
      reachability.Mark(NodeKind::Function, static_cast<std::uint32_t>(index), TypeExposure::Host);
    }
  }
  for (const auto initializer : program.global_initializers) {
    reachability.Mark(NodeKind::Function, initializer);
  }
  if (main) {
    reachability.Mark(NodeKind::Function, *main);
  }
  reachability.Trace();
  reachability.Prune(program, main);
}

}  // namespace vela::compiler::aot
