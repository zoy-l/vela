#include "compiler/frontend/typing/type_checker.h"

#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include "compiler/frontend/builtins/standard_environment.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/typing/semantic_model.h"

namespace vela::frontend {
namespace {

using MemberKind = ReflectionMemberKind;

bool IsReflectionNominal(const Program& program,
                         const TypeDescriptor& type,
                         std::string_view module) {
  const auto standard_module = module.starts_with(kStandardSourcePrefix);
  const auto standard = standard_module && [&] {
    const auto origin = program.origins.find(std::string(type.name));
    return origin != program.origins.end() && origin->second.module_uri == module;
  }();
  return !type.nullable &&
         (standard || (!standard_module && (type.kind == TypeDescriptor::Kind::Class ||
                                            type.kind == TypeDescriptor::Kind::Interface ||
                                            type.kind == TypeDescriptor::Kind::Enum)));
}

}  // namespace

struct TypeChecker::ReflectionDeclarations final {
  absl::flat_hash_set<std::string> fields;
  absl::flat_hash_map<std::pair<MemberKind, std::string>, const Function*> methods;
  const Class* owner = nullptr;

  explicit ReflectionDeclarations(const Class& source) : owner(&source) {
    for (const auto& field : source.fields) {
      fields.emplace(field.name);
    }
    AddMethods(source.methods);
    AddMethods(source.method_templates);
  }
  explicit ReflectionDeclarations(const Interface* source) {
    if (source != nullptr) {
      AddMethods(source->methods);
      AddMethods(source->method_templates);
    }
  }
  std::expected<const Function*, std::string> Find(
      const ReflectionMemberSelection& selection) const;

  void AddMethods(std::span<const Function> declarations) {
    for (const auto& method : declarations) {
      if (!method.application) {
        methods.emplace(std::pair(ReflectedMethodKind(method), method.name), &method);
      }
    }
  }
};

std::expected<const Function*, std::string> TypeChecker::ReflectionDeclarations::Find(
    const ReflectionMemberSelection& selection) const {
  if (selection.kind != MemberKind::Method && !selection.type_arguments.empty()) {
    return std::unexpected("only a method selection accepts type arguments");
  }
  switch (selection.kind) {
    case MemberKind::Field:
      if (fields.contains(selection.name)) {
        return nullptr;
      }
      break;
    case MemberKind::Constructor:
      if (!selection.name.empty()) {
        return std::unexpected("constructor selection has no name");
      }
      if (owner != nullptr && owner->constructor_kind != ConstructorKind::Unavailable) {
        return owner->constructor.get();
      }
      break;
    case MemberKind::Getter:
    case MemberKind::Setter:
    case MemberKind::Method: {
      const auto found = methods.find({selection.kind, selection.name});
      if (found != methods.end()) {
        return found->second;
      }
      break;
    }
    case MemberKind::EnumMember:
      break;
  }
  return std::unexpected("selected member is not declared with this kind on the registered type");
}

std::optional<ReflectionMemberBinding> TypeChecker::BindReflectionMethod(
    const Type& owner,
    const ReflectionMemberSelection& selection,
    const Function& declaration) {
  ReflectionMemberBinding binding{
      .kind = selection.kind,
      .name = selection.name,
      .type_arguments = {},
  };
  const auto owner_name = std::string(owner.name);
  auto arguments =
      ApplyMethod({.owner = owner_name, .declaration = &declaration}, selection.type_arguments,
                  selection.location, "reflection method type argument count does not match");
  if (!arguments) {
    return std::nullopt;
  }
  binding.type_arguments = std::move(*arguments);
  return binding;
}

void TypeChecker::BindReflectionMembers(ReflectionBinding& binding,
                                        std::span<const ReflectionMemberSelection> members,
                                        const ReflectionDeclarations& declarations) {
  const auto& type = binding.type;
  for (const auto& member : members) {
    if (type.kind == Type::Kind::Enum && member.kind == MemberKind::EnumMember &&
        member.type_arguments.empty() &&
        enum_values_.contains(std::string(type.name) + '.' + member.name)) {
      binding.members.push_back({.kind = member.kind, .name = member.name, .type_arguments = {}});
      continue;
    }
    const auto declaration = declarations.Find(member);
    if (!declaration) {
      AddDiagnostic(member.location, declaration.error());
    } else if (member.kind == MemberKind::Method) {
      if (auto method = BindReflectionMethod(type, member, **declaration)) {
        binding.members.push_back(std::move(*method));
      }
    } else {
      binding.members.push_back({.kind = member.kind, .name = member.name, .type_arguments = {}});
    }
  }
}

void TypeChecker::CheckReflectionSelections(const Program& program) {
  absl::flat_hash_map<std::string, ReflectionDeclarations> declarations;
  for (const auto& selection : program.reflection) {
    const auto diagnostic_begin = diagnostics_.size();
    const auto type = ResolveType(selection.type, VoidTypePolicy::Forbidden);
    if (!IsReflectionNominal(program, type, selection.module)) {
      AddDiagnostic(selection.location, "reflection registration requires a concrete nominal type");
    } else {
      const auto name = std::string(type.name);
      auto found = declarations.find(name);
      if (found == declarations.end()) {
        if (const auto owner = classes_.find(type.name); owner != classes_.end()) {
          found = declarations.try_emplace(name, *owner->second.declaration).first;
        } else {
          const auto interface = interfaces_.find(type.name);
          found =
              declarations
                  .try_emplace(name, interface == interfaces_.end() ? nullptr
                                                                    : interface->second.declaration)
                  .first;
        }
      }
      ReflectionBinding binding{.type = type, .members = {}};
      BindReflectionMembers(binding, selection.members, found->second);
      reflection_.push_back(std::move(binding));
    }
    for (auto& diagnostic : std::span(diagnostics_).subspan(diagnostic_begin)) {
      diagnostic.source_path =
          selection.source_path.empty() ? selection.module : selection.source_path;
      if (diagnostic.location == SourceLocation{}) {
        diagnostic.location = selection.location;
      }
    }
  }
}

}  // namespace vela::frontend
