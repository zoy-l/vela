#include "compiler/frontend/typing/type_checker.h"

#include <cstdint>
#include <optional>
#include <string_view>

#include <absl/container/flat_hash_set.h>

#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/syntax/names.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/frontend/typing/type_names.h"

namespace vela::frontend {

void TypeChecker::ResolveStructLayouts() {
  absl::flat_hash_set<std::string_view> visiting;
  absl::flat_hash_set<std::string_view> complete;
  for (const auto& [name, owner] : classes_) {
    if (owner.type.value_layout != nullptr && !owner.type.is_abstract) {
      ResolveStructLayout(owner, visiting, complete);
    }
  }
}

void TypeChecker::ResolveStructLayout(const ClassInfo& owner,
                                      absl::flat_hash_set<std::string_view>& visiting,
                                      absl::flat_hash_set<std::string_view>& complete) {
  const auto name = owner.type.name;
  if (complete.contains(name)) {
    return;
  }
  visiting.insert(name);
  auto& layout = *owner.type.value_layout;
  for (const auto& field_name : owner.field_order) {
    const auto& field = owner.fields.at(field_name);
    const auto& type = field.type;
    if (type.value_layout != nullptr) {
      const auto nested_name = NonNullableTypeName(type);
      if (visiting.contains(nested_name)) {
        AddDiagnostic(field.declaration->location, "struct field creates a recursive value layout");
        continue;
      }
      ResolveStructLayout(classes_.at(nested_name), visiting, complete);
    }
    layout.offsets.push_back(static_cast<std::uint32_t>(layout.slots.size()));
    layout.fields.push_back(type);
    if (type.value_layout == nullptr) {
      layout.slots.push_back(type);
      continue;
    }
    // 可空的嵌套值有独立存在位，不能把引用字段的 null 当作整个值缺失。
    if (type.nullable) {
      layout.slots.push_back({.kind = Type::Kind::Bool, .name = kBoolTypeName});
    }
    const auto& nested_slots = type.value_layout->slots;
    layout.slots.insert(layout.slots.end(), nested_slots.begin(), nested_slots.end());
  }
  visiting.erase(name);
  complete.insert(name);
}

std::optional<TypeChecker::StructMemberField> TypeChecker::FindStructMemberField(
    const Expression& expression) const {
  if (const auto found = facts_.static_members.find(&expression);
      found != facts_.static_members.end()) {
    const auto& owner = classes_.at(found->second.owner);
    const auto field = owner.static_fields.find(found->second.member);
    return field == owner.static_fields.end()
               ? std::nullopt
               : std::optional(StructMemberField{.field = &field->second, .receiver = nullptr});
  }
  const auto found = facts_.expression_types.find(expression.object.get());
  if (found == facts_.expression_types.end() || found->second.kind != Type::Kind::Class) {
    return std::nullopt;
  }
  const auto& receiver = found->second;
  const auto* field = classes_.at(NonNullableTypeName(receiver)).FindField(expression.text);
  return field == nullptr ? std::nullopt
                          : std::optional(StructMemberField{.field = field, .receiver = &receiver});
}

void TypeChecker::CheckStructWriteTarget(const Expression& target) {
  if (target.kind == Expression::Kind::Name) {
    if (target.text != kReceiverName) {
      static_cast<void>(CheckAssignedBinding(target));
    }
    return;
  }
  if (target.kind == Expression::Kind::Index) {
    return;
  }
  if (target.kind == Expression::Kind::Member && FindStructMemberField(target)) {
    CheckMemberAssignment(target);
    return;
  }
  AddDiagnostic(target.location, "cannot modify a field of a temporary struct value");
}

bool TypeChecker::IsMutableStructPlace(const Expression& expression) const {
  if (expression.kind == Expression::Kind::Name) {
    if (expression.text == kReceiverName) {
      return true;
    }
    if (const auto* local = LookupLocal(expression.text)) {
      return local->kind == BindingKind::Mutable;
    }
    const auto global = globals_.find(expression.text);
    return global != globals_.end() && global->second.declaration->binding == BindingKind::Mutable;
  }
  if (expression.kind == Expression::Kind::Index) {
    return facts_.expression_types.at(expression.object.get()).kind == Type::Kind::Array;
  }
  if (expression.kind != Expression::Kind::Member) {
    return false;
  }
  const auto member = FindStructMemberField(expression);
  return member && member->field->binding == BindingKind::Mutable &&
         (member->receiver == nullptr || member->receiver->value_layout == nullptr ||
          IsMutableStructPlace(*expression.object));
}

}  // namespace vela::frontend
