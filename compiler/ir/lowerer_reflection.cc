#include "compiler/ir/lowerer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/node_hash_map.h>

#include "base/callable.h"
#include "base/declaration.h"
#include "base/reflection.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/syntax/names.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/ir/declaration_bindings.h"
#include "compiler/ir/ir.h"

namespace vela::ir {
namespace {

using reflection::MemberKind;
using SelectionKind = frontend::ReflectionMemberKind;

MemberKind ReflectKind(SelectionKind kind) {
  switch (kind) {
    case SelectionKind::Field:
      return MemberKind::Field;
    case SelectionKind::Getter:
      return MemberKind::Getter;
    case SelectionKind::Setter:
      return MemberKind::Setter;
    case SelectionKind::Method:
      return MemberKind::Method;
    case SelectionKind::Constructor:
      return MemberKind::Constructor;
    case SelectionKind::EnumMember:
      return MemberKind::EnumMember;
  }
  std::unreachable();
}

std::uint32_t MemberFlags(frontend::Visibility visibility, frontend::MemberStorage storage) {
  return (visibility == frontend::Visibility::Private ? reflection::kPrivate : 0U) |
         (storage == frontend::MemberStorage::Static ? reflection::kStatic : 0U);
}

using MemberKey = std::tuple<std::uint32_t, MemberKind, std::string, std::vector<std::uint32_t>>;

reflection::Member DescribeMethodTemplate(const frontend::Function& method,
                                          DeclarationId declaration,
                                          std::uint32_t declaring_type) {
  return {
      .declaration = declaration,
      .declaring_type = declaring_type,
      .name = method.name,
      .kind = MemberKind::Method,
      .flags = MemberFlags(method.visibility, method.storage),
      .getter = {},
      .setter = {},
      .signature = {},
      .default_parameters = {},
      .type_arguments = {},
  };
}

void RegisterProperties(std::span<reflection::Member> members) {
  absl::flat_hash_map<DeclarationId, std::uint32_t> accessors;
  for (const auto& member : members) {
    if (member.kind == MemberKind::Getter || member.kind == MemberKind::Setter) {
      accessors.emplace(member.declaration, member.flags);
    }
  }
  for (auto& member : members) {
    if (member.kind != MemberKind::Property) {
      continue;
    }
    bool registered = false;
    bool public_accessor = false;
    bool public_registered_accessor = false;
    for (const auto declaration : {member.getter, member.setter}) {
      if (const auto found = accessors.find(declaration); found != accessors.end()) {
        const auto flags = found->second;
        const auto selected = (flags & reflection::kRegistered) != 0;
        const auto visible = (flags & reflection::kPrivate) == 0;
        registered |= selected;
        public_accessor |= visible;
        public_registered_accessor |= selected && visible;
      }
    }
    member.flags &= ~(reflection::kRegistered | reflection::kPrivate);
    if (registered) {
      member.flags |= reflection::kRegistered;
    }
    if (!(registered ? public_registered_accessor : public_accessor)) {
      member.flags |= reflection::kPrivate;
    }
  }
}

}  // namespace

std::uint32_t Lowerer::DescribeReflectionArgument(const frontend::TypeDescriptor& type) {
  return type.kind == frontend::TypeDescriptor::Kind::Void ? kInvalidFunctionTarget
                                                           : RegisterRuntimeType(type);
}

std::vector<std::uint32_t> Lowerer::DescribeReflectionArguments(
    std::span<const frontend::TypeDescriptor> arguments) {
  std::vector<std::uint32_t> result;
  result.reserve(arguments.size());
  for (const auto& argument : arguments) {
    result.push_back(DescribeReflectionArgument(argument));
  }
  return result;
}

reflection::Type Lowerer::DescribeReflectionType(const frontend::TypeDescriptor& type) {
  const auto& origin = source_.origins.at(type.name);
  reflection::Type result{
      .declaration = declarations_->nominals.at(type.name),
      .type = RegisterRuntimeType(type),
      .module = origin.module_uri,
      .name = origin.name,
      .arguments = {},
      .interfaces = {},
  };
  if (!origin.arguments.empty()) {
    result.arguments = DescribeReflectionArguments(model_.generic_arguments.at(type.name));
  }
  if (type.kind == frontend::TypeDescriptor::Kind::Class) {
    const auto& owner = model_.classes.at(type.name);
    if (owner.base_class) {
      result.base = RegisterRuntimeType(model_.classes.at(*owner.base_class).type);
    }
    std::vector<std::string> interfaces(owner.interfaces.begin(), owner.interfaces.end());
    std::ranges::sort(interfaces);
    for (const auto& name : interfaces) {
      result.interfaces.push_back(RegisterRuntimeType(model_.interfaces.at(name).type));
    }
  }
  return result;
}

reflection::Member Lowerer::DescribeReflectionMethod(
    const frontend::TypeDescriptor& owner,
    std::uint32_t declaring_type,
    const frontend::Function& source,
    const absl::node_hash_map<std::string, frontend::FunctionDescriptor>& methods) {
  const auto interface = owner.kind == frontend::TypeDescriptor::Kind::Interface;
  const auto key = interface ? QualifiedMemberKey({.owner = owner.name, .name = source.name})
                             : MethodKey(owner.name, source);
  const auto target = interface ? interface_method_ids_.at(key) : function_ids_.at(key);
  const auto kind = ReflectKind(frontend::ReflectedMethodKind(source));
  const auto declaration = interface ? declarations_->selectors.at(key)
                                     : declarations_->functions.at(&source).declaration;
  const auto* signature = interface || source.application ? &methods.at(source.name) : nullptr;
  CallSignature call_signature;
  if (interface) {
    call_signature = DescribeCallSignature(*signature);
  } else {
    const auto& native_signature = program_.functions.at(target).signature;
    if (!native_signature) {
      std::unreachable();
    }
    call_signature = *native_signature;
  }
  reflection::Member result{
      .declaration = declaration,
      .declaring_type = declaring_type,
      .name = frontend::ReflectedMethodName(source),
      .kind = kind,
      .flags = MemberFlags(source.visibility, source.storage) |
               (source.is_async ? reflection::kAsync : 0U),
      .target = target,
      .getter = {},
      .setter = {},
      .signature = std::move(call_signature),
      .default_parameters = {},
      .type_arguments = {},
  };
  result.signature.hidden_parameters = 0;
  result.signature.receiver.reset();
  result.type = result.signature.result.value_or(kInvalidFunctionTarget);
  if (source.application) {
    result.type_arguments = DescribeReflectionArguments(signature->type_arguments);
  }
  return result;
}

void Lowerer::DescribeReflectionFields(const frontend::Class& owner,
                                       const frontend::ClassDescriptor& descriptor,
                                       const ClassDeclarations& identities,
                                       std::uint32_t type) {
  for (const auto& field : owner.fields) {
    const auto is_static = field.storage == frontend::MemberStorage::Static;
    const auto key =
        is_static ? QualifiedMemberKey({.owner = owner.name, .name = field.name}) : std::string{};
    const auto& contract =
        is_static ? descriptor.static_fields.at(field.name) : descriptor.fields.at(field.name);
    program_.reflection_members.push_back({
        .declaration =
            is_static ? declarations_->globals.at(key) : identities.fields.at(field.name),
        .declaring_type = type,
        .name = field.name,
        .kind = MemberKind::Field,
        .flags = MemberFlags(field.visibility, field.storage),
        .type = RegisterRuntimeType(contract.type),
        .target = is_static ? global_ids_.at(key) : contract.index,
        .getter = {},
        .setter = {},
        .signature = {},
        .default_parameters = {},
        .type_arguments = {},
    });
  }
}

void Lowerer::DescribeReflectionClass(const frontend::Class& owner) {
  const auto& descriptor = model_.classes.at(owner.name);
  const auto& identities = declarations_->classes.at(owner.name);
  const auto type = RegisterRuntimeType(descriptor.type);
  DescribeReflectionFields(owner, descriptor, identities, type);
  absl::flat_hash_map<std::string, std::size_t> properties;
  for (const auto& method : owner.methods) {
    auto member = DescribeReflectionMethod(descriptor.type, type, method, descriptor.methods);
    if (method.is_getter || method.is_setter) {
      const auto [position, inserted] =
          properties.try_emplace(method.name, program_.reflection_members.size());
      if (inserted) {
        program_.reflection_members.push_back({
            .declaration = identities.properties.at(method.name),
            .declaring_type = type,
            .name = method.name,
            .kind = MemberKind::Property,
            .flags = member.flags,
            .type = method.is_getter ? member.type : member.signature.parameters.front().type,
            .getter = {},
            .setter = {},
            .signature = {},
            .default_parameters = {},
            .type_arguments = {},
        });
      }
      auto& property = program_.reflection_members.at(position->second);
      (method.is_getter ? property.getter : property.setter) = member.declaration;
    }
    program_.reflection_members.push_back(std::move(member));
  }
  for (const auto& method : owner.method_templates) {
    program_.reflection_members.push_back(DescribeMethodTemplate(
        method, declarations_->named_functions.at(MethodKey(owner.name, method)), type));
  }
  DescribeReflectionConstructor(owner, descriptor, type);
}

void Lowerer::DescribeReflectionConstructor(const frontend::Class& owner,
                                            const frontend::ClassDescriptor& descriptor,
                                            std::uint32_t type) {
  if (owner.constructor_kind == frontend::ConstructorKind::Unavailable) {
    return;
  }
  const auto visibility =
      owner.constructor ? owner.constructor->visibility : frontend::Visibility::Public;
  reflection::Member constructor{
      .declaration = declarations_->named_functions.at(
          MethodKey(owner.name, DeclarationKind::Method, frontend::kConstructorName)),
      .declaring_type = type,
      .name = std::string(reflection::kConstructorName),
      .kind = MemberKind::Constructor,
      .flags = MemberFlags(visibility, frontend::MemberStorage::Instance),
      .type = type,
      .target = program_.classes.at(class_ids_.at(owner.name)).constructor,
      .getter = {},
      .setter = {},
      .signature = DescribeCallSignature(descriptor.constructor),
      .default_parameters = {},
      .type_arguments = {},
  };
  constructor.signature.result = type;
  if (owner.constructor_kind == frontend::ConstructorKind::Generated) {
    for (std::size_t index = 0; index < constructor.signature.parameters.size(); ++index) {
      if (constructor.signature.parameters.at(index).optional) {
        constructor.default_parameters.push_back(static_cast<std::uint32_t>(index));
      }
    }
  }
  program_.reflection_members.push_back(std::move(constructor));
}

void Lowerer::DescribeReflectionInterface(const frontend::Interface& owner) {
  const auto& descriptor = model_.interfaces.at(owner.name);
  const auto& type = descriptor.type;
  const auto declaring_type = RegisterRuntimeType(type);
  for (const auto& method : owner.methods) {
    program_.reflection_members.push_back(
        DescribeReflectionMethod(type, declaring_type, method, descriptor.methods));
  }
  for (const auto& method : owner.method_templates) {
    program_.reflection_members.push_back(DescribeMethodTemplate(
        method,
        declarations_->selectors.at(QualifiedMemberKey({.owner = owner.name, .name = method.name})),
        declaring_type));
  }
}

void Lowerer::DescribeReflectionEnum(const frontend::Enum& owner) {
  const auto type = RegisterRuntimeType(model_.types.at(owner.name));
  const auto& identities = declarations_->enum_members.at(owner.name);
  for (std::size_t ordinal = 0; ordinal < owner.members.size(); ++ordinal) {
    const auto& member = owner.members.at(ordinal);
    program_.reflection_members.push_back({
        .declaration = identities.at(member.name),
        .declaring_type = type,
        .name = member.name,
        .kind = MemberKind::EnumMember,
        .flags = reflection::kStatic,
        .type = type,
        .target = static_cast<std::uint32_t>(ordinal),
        .getter = {},
        .setter = {},
        .signature = {},
        .default_parameters = {},
        .type_arguments = {},
    });
  }
}

void Lowerer::LowerReflection() {
  // 未登记的名义类型仍须支持 typeInfo/typeOf；成员目录由显式登记单独决定。
  for (const auto& owner : source_.classes) {
    program_.reflection_types.push_back(DescribeReflectionType(model_.classes.at(owner.name).type));
  }
  for (const auto& owner : source_.interfaces) {
    program_.reflection_types.push_back(
        DescribeReflectionType(model_.interfaces.at(owner.name).type));
  }
  for (const auto& owner : source_.enums) {
    program_.reflection_types.push_back(DescribeReflectionType(model_.types.at(owner.name)));
  }
  if (model_.reflection.empty()) {
    LowerAttributes();
    return;
  }
  for (const auto& owner : source_.classes) {
    DescribeReflectionClass(owner);
  }
  for (const auto& owner : source_.interfaces) {
    DescribeReflectionInterface(owner);
  }
  for (const auto& owner : source_.enums) {
    DescribeReflectionEnum(owner);
  }
  RegisterReflectionSelections();
  RegisterProperties(program_.reflection_members);
  LowerAttributes();
}

void Lowerer::LowerAttributes() {
  if (model_.attributes.empty()) {
    return;
  }
  for (const auto& attribute : model_.attributes) {
    reflection::Attribute entry{
        .target = declarations_->attribute_targets.at(attribute.target),
        .type = RegisterRuntimeType(attribute.type),
        .fields = {},
    };
    for (const auto& field : attribute.fields) {
      entry.fields.push_back({.type = RegisterRuntimeType(field.type), .value = field.value});
    }
    program_.attributes.push_back(std::move(entry));
  }
}

void Lowerer::RegisterReflectionSelections() {
  absl::flat_hash_map<std::uint32_t, std::size_t> types;
  for (std::size_t index = 0; index < program_.reflection_types.size(); ++index) {
    types.emplace(program_.reflection_types.at(index).type, index);
  }
  absl::flat_hash_map<MemberKey, std::size_t> members;
  for (std::size_t index = 0; index < program_.reflection_members.size(); ++index) {
    const auto& member = program_.reflection_members.at(index);
    members.emplace(
        MemberKey{member.declaring_type, member.kind, member.name, member.type_arguments}, index);
  }
  for (const auto& selection : model_.reflection) {
    const auto type = RegisterRuntimeType(selection.type);
    program_.reflection_types.at(types.at(type)).flags |= reflection::kRegistered;
    for (const auto& member : selection.members) {
      const auto arguments = DescribeReflectionArguments(member.type_arguments);
      const auto name = member.kind == SelectionKind::Constructor
                            ? std::string(reflection::kConstructorName)
                            : member.name;
      program_.reflection_members
          .at(members.at(MemberKey{type, ReflectKind(member.kind), name, arguments}))
          .flags |= reflection::kRegistered;
    }
  }
}

}  // namespace vela::ir
