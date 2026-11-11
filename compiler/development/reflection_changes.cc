#include "compiler/development/reflection_changes.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/functional/function_ref.h>

#include "base/callable.h"
#include "base/declaration.h"
#include "base/layout_description.h"
#include "base/reflection.h"
#include "compiler/development/development_session.h"
#include "compiler/ir/ir.h"

namespace vela::compiler {
namespace {

bool IsSameType(const ir::Program& before,
                std::uint32_t left,
                const ir::Program& after,
                std::uint32_t right) {
  if (left == kInvalidFunctionTarget || right == kInvalidFunctionTarget) {
    return left == right;
  }
  return layout::IsSameType(before.layouts, before.layouts.types[left], after.layouts,
                            after.layouts.types[right]);
}

bool IsSameMember(const ir::Program& before,
                  const reflection::Member& left,
                  const ir::Program& after,
                  const reflection::Member& right) {
  if (left.kind != right.kind || left.name != right.name || left.flags != right.flags ||
      left.getter != right.getter || left.setter != right.setter ||
      !IsSameType(before, left.type, after, right.type) ||
      left.default_parameters != right.default_parameters) {
    return false;
  }
  const auto& previous = left.signature;
  const auto& current = right.signature;
  return IsSameType(before, previous.result.value_or(kInvalidFunctionTarget), after,
                    current.result.value_or(kInvalidFunctionTarget)) &&
         std::ranges::equal(
             previous.parameters, current.parameters, [&](const auto& first, const auto& second) {
               return first.name == second.name && first.optional == second.optional &&
                      first.rest == second.rest &&
                      IsSameType(before, first.type, after, second.type);
             });
}

using AttributeTargets =
    absl::flat_hash_map<DeclarationId, std::vector<const reflection::Attribute*>>;

AttributeTargets IndexAttributes(const ir::Program& program) {
  AttributeTargets targets;
  for (const auto& attribute : program.attributes) {
    targets[attribute.target].push_back(&attribute);
  }
  return targets;
}

std::span<const reflection::Attribute* const> AttributesAt(const AttributeTargets& targets,
                                                           DeclarationId declaration) {
  const auto found = targets.find(declaration);
  return found == targets.end() ? std::span<const reflection::Attribute* const>{} : found->second;
}

bool IsSameAttribute(const ir::Program& before,
                     const reflection::Attribute& left,
                     const ir::Program& after,
                     const reflection::Attribute& right) {
  if (!IsSameType(before, left.type, after, right.type)) {
    return false;
  }
  const auto& previous = before.classes.at(before.layouts.types.at(left.type).index).fields;
  const auto& current = after.classes.at(after.layouts.types.at(right.type).index).fields;
  return std::ranges::equal(previous, current,
                            [&](const auto& first, const auto& second) {
                              return first.declaration == second.declaration &&
                                     IsSameType(before, first.type, after, second.type);
                            }) &&
         std::ranges::equal(left.fields, right.fields, [&](const auto& first, const auto& second) {
           if (!IsSameType(before, first.type, after, second.type) ||
               first.value.index() != second.value.index()) {
             return false;
           }
           if (const auto* value = std::get_if<double>(&first.value)) {
             return std::bit_cast<std::uint64_t>(*value) ==
                    std::bit_cast<std::uint64_t>(std::get<double>(second.value));
           }
           return first.value == second.value;
         });
}

struct ReflectionDelta final {
  std::vector<DeclarationId>* registered;
  std::vector<DeclarationId>* withdrawn;
  std::vector<DeclarationId>* deleted;
};

template <class Entry>
struct PublicationEntries final {
  std::span<const Entry> before;
  std::span<const Entry> after;
};

template <class Entry, class Key>
void Compare(
    PublicationEntries<Entry> entries,
    std::span<const ir::Declaration> declarations,
    const Key& key,
    ReflectionDelta delta,
    std::optional<absl::FunctionRef<void(const Entry&, const Entry&)>> changed = std::nullopt) {
  absl::flat_hash_map<DeclarationId, const Entry*> previous;
  for (const auto& entry : entries.before) {
    previous.emplace(key(entry), &entry);
  }
  for (const auto& entry : entries.after) {
    const auto identity = key(entry);
    const auto found = previous.find(identity);
    const auto selected = (entry.flags & reflection::kRegistered) != 0;
    if (found == previous.end()) {
      if (selected) {
        delta.registered->push_back(identity);
      }
      continue;
    }
    const auto& before = *found->second;
    const auto was_selected = (before.flags & reflection::kRegistered) != 0;
    if (selected && !was_selected) {
      delta.registered->push_back(identity);
    }
    if (!selected && was_selected) {
      delta.withdrawn->push_back(identity);
    }
    if (changed && (selected || was_selected)) {
      (*changed)(before, entry);
    }
    previous.erase(found);
  }
  for (const auto& [identity, entry] : previous) {
    if ((entry->flags & reflection::kRegistered) != 0) {
      const auto exists = FindDeclaration(declarations, identity) != nullptr;
      (exists ? delta.withdrawn : delta.deleted)->push_back(identity);
    }
  }
  for (auto* values : {delta.registered, delta.withdrawn, delta.deleted}) {
    std::ranges::sort(*values, {}, &DeclarationId::value);
  }
}

}  // namespace

void DescribeReflectionChanges(const ir::Program& previous,
                               const ir::Program& current,
                               DevelopmentChanges& changes) {
  const auto previous_attributes = IndexAttributes(previous);
  const auto current_attributes = IndexAttributes(current);
  Compare<reflection::Type>(
      {.before = previous.reflection_types, .after = current.reflection_types},
      current.declarations, [](const auto& entry) { return entry.declaration; },
      {
          .registered = &changes.registered_types,
          .withdrawn = &changes.withdrawn_types,
          .deleted = &changes.deleted_types,
      });
  const auto describe_member = [&](const reflection::Member& before,
                                   const reflection::Member& after) {
    if (!IsSameMember(previous, before, current, after) ||
        !std::ranges::equal(AttributesAt(previous_attributes, before.declaration),
                            AttributesAt(current_attributes, after.declaration),
                            [&](const auto* first, const auto* second) {
                              return IsSameAttribute(previous, *first, current, *second);
                            })) {
      changes.changed_members.push_back(after.declaration);
    }
  };
  Compare<reflection::Member>(
      {.before = previous.reflection_members, .after = current.reflection_members},
      current.declarations, [](const auto& entry) { return entry.declaration; },
      {
          .registered = &changes.registered_members,
          .withdrawn = &changes.withdrawn_members,
          .deleted = &changes.deleted_members,
      },
      absl::FunctionRef<void(const reflection::Member&, const reflection::Member&)>(
          describe_member));
  std::ranges::sort(changes.changed_members, {}, &DeclarationId::value);
}

}  // namespace vela::compiler
