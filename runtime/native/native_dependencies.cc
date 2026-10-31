#include "runtime/native/native_contracts.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include "base/declaration.h"
#include "runtime/native/native_image.h"
#include "runtime/native/native_version.h"

namespace vela::runtime {
namespace {

using DeclarationIndex = absl::flat_hash_map<DeclarationId, const NativeDeclarationDescription*>;

enum class DependencyGroup : std::uint8_t { Field, Global, Version, Construction, Binding, Count };

class DependencySelection final {
 public:
  DependencySelection(const NativeFunctionDependencies& dependencies,
                      std::uint32_t instruction,
                      NativeDependencyPhase phase)
      : positions_(dependencies.positions.values()),
        blocks_(dependencies.blocks.values()),
        fully_reached_(blocks_.size()),
        instruction_(instruction),
        phase_(phase) {
    const std::array counts{
        dependencies.fields.size,        dependencies.globals.size,  dependencies.versions.size,
        dependencies.constructions.size, dependencies.bindings.size,
    };
    std::uint64_t offset = 0;
    for (std::size_t index = 0; index < counts.size(); ++index) {
      offsets_.at(index) = offset;
      offset += counts.at(index);
    }
    assert(offset == positions_.size());
    if (blocks_.empty()) {
      return;
    }
    first_ = BlockAt(instruction);
    assert(instruction < blocks_.subspan(first_).front().end);
    // 首块只从当前指令开始；后继绕回首块时，才将其早先的依赖也纳入可达集合。
    const auto successors = blocks_.subspan(first_).front().successors.values();
    std::vector<std::uint32_t> pending(successors.begin(), successors.end());
    while (!pending.empty()) {
      const auto index = pending.back();
      pending.pop_back();
      auto& reached = fully_reached_.at(index);
      if (reached != 0) {
        continue;
      }
      reached = 1;
      const auto next = blocks_.subspan(index).front().successors.values();
      pending.insert(pending.end(), next.begin(), next.end());
    }
  }

  [[nodiscard]] bool CanReach(DependencyGroup group, std::size_t index) const {
    const auto ordinal = offsets_.at(static_cast<std::size_t>(group)) + index;
    const auto position = positions_.subspan(static_cast<std::size_t>(ordinal)).front();
    const auto block = BlockAt(position);
    if (phase_ == NativeDependencyPhase::AwaitingResult && group == DependencyGroup::Binding &&
        position == instruction_ && fully_reached_.at(block) == 0) {
      // 父帧已选定这次调用的目标；只有循环绕回时才需要保留下一次查找的依赖。
      return false;
    }
    return fully_reached_.at(block) != 0 || (block == first_ && position >= instruction_);
  }

 private:
  [[nodiscard]] std::size_t BlockAt(std::uint32_t instruction) const {
    const auto after =
        std::ranges::upper_bound(blocks_, instruction, {}, &NativeDependencyBlock::begin);
    return static_cast<std::size_t>(after - blocks_.begin() - 1);
  }
  std::span<const std::uint32_t> positions_;
  std::span<const NativeDependencyBlock> blocks_;
  std::vector<std::uint8_t> fully_reached_;
  std::array<std::uint64_t, static_cast<std::size_t>(DependencyGroup::Count)> offsets_{};
  std::size_t first_ = 0;
  std::uint32_t instruction_;
  NativeDependencyPhase phase_;
};

struct MemberName final {
  DeclarationId owner;
  std::string_view name;
  bool operator==(const MemberName&) const = default;
  template <class H>
  friend H AbslHashValue(H state, const MemberName& member) {
    return H::combine(std::move(state), member.owner, member.name);
  }
};

class RetainedContracts final {
 public:
  RetainedContracts(const NativeImage* previous, const NativeImage& candidate);
  std::expected<void, NativeContractViolation> Check(const NativeFunctionDescription& function,
                                                     const DeclarationIndex& source,
                                                     const DependencySelection& selection) const;

 private:
  static NativeContractViolation Violation(const NativeFunctionDescription& function,
                                           DeclarationId declaration,
                                           std::string message);
  std::expected<void, NativeContractViolation> CheckField(const NativeFunctionDescription& function,
                                                          DeclarationId field) const;
  std::expected<void, NativeContractViolation> CheckGlobal(
      const NativeFunctionDescription& function,
      DeclarationId global) const;
  std::expected<void, NativeContractViolation> CheckConstruction(
      const NativeFunctionDescription& function,
      const NativeConstructionDescription& construction) const;
  std::expected<void, NativeContractViolation> CheckBinding(
      const NativeFunctionDescription& function,
      const NativeBindingDependency& binding,
      const DeclarationIndex& source) const;
  [[nodiscard]] const NativeDeclarationDescription* FindMember(
      DeclarationId owner,
      const NativeDeclarationDescription& original) const;

  absl::flat_hash_map<DeclarationId, const NativeClassDescription*> classes_;
  absl::flat_hash_map<DeclarationId, const NativeFieldDescription*> fields_;
  absl::flat_hash_map<DeclarationId, std::uint32_t> globals_;
  DeclarationIndex declarations_;
  absl::flat_hash_set<DeclarationId> previous_declarations_;
  absl::flat_hash_map<MemberName, std::vector<const NativeDeclarationDescription*>> names_;
  absl::flat_hash_map<DeclarationId, const NativeFunctionDescription*> functions_;
};

RetainedContracts::RetainedContracts(const NativeImage* previous, const NativeImage& candidate) {
  if (previous != nullptr) {
    for (const auto& declaration : previous->declarations.values()) {
      previous_declarations_.insert(declaration.identity);
    }
  }
  for (const auto& declaration : candidate.declarations.values()) {
    declarations_.emplace(declaration.identity, &declaration);
    names_.try_emplace(MemberName{.owner = declaration.owner, .name = declaration.name.text()})
        .first->second.push_back(&declaration);
  }
  for (const auto& function : candidate.functions.values()) {
    if (function.identity.local == 0) {
      functions_.emplace(function.identity.declaration, &function);
    }
  }
  for (const auto& type : candidate.classes.values()) {
    if (type.declaration != DeclarationId{}) {
      classes_.emplace(type.declaration, &type);
    }
    for (const auto& field : type.fields.values()) {
      fields_.emplace(field.declaration, &field);
    }
  }
  const auto globals = candidate.global_declarations.values();
  const auto flags = candidate.global_flags.values();
  for (std::size_t index = 0; index < globals.size(); ++index) {
    globals_.emplace(globals.subspan(index).front(), flags.subspan(index).front());
  }
}

NativeContractViolation RetainedContracts::Violation(const NativeFunctionDescription& function,
                                                     DeclarationId declaration,
                                                     std::string message) {
  return {
      .declaration = declaration,
      .message = "retained function '" + std::string(function.name.text()) + "' (revision " +
                 std::to_string(function.identity.revision) + ") " + std::move(message),
      .dependency = function.identity,
  };
}

std::expected<void, NativeContractViolation> RetainedContracts::CheckConstruction(
    const NativeFunctionDescription& function,
    const NativeConstructionDescription& construction) const {
  const auto found = classes_.find(construction.type);
  if (found == classes_.end()) {
    return std::unexpected(
        Violation(function, construction.type, "still constructs a deleted class"));
  }
  const auto supplied = construction.supplied.values();
  const absl::flat_hash_set<DeclarationId> provided(supplied.begin(), supplied.end());
  for (const auto& field : found->second->fields.values()) {
    if (field.initializer == kNoNativeIndex && !provided.contains(field.declaration)) {
      return std::unexpected(Violation(function, field.declaration,
                                       "omits a field which is now required; Reload is required"));
    }
  }
  return {};
}

const NativeDeclarationDescription* RetainedContracts::FindMember(
    DeclarationId owner,
    const NativeDeclarationDescription& original) const {
  const auto kind = static_cast<DeclarationKind>(original.kind);
  while (owner != DeclarationId{}) {
    const auto name = names_.find(MemberName{.owner = owner, .name = original.name.text()});
    if (name != names_.end()) {
      for (const auto* member : name->second) {
        if (member->kind == original.kind) {
          return member;
        }
      }
      // getter/setter 共用属性名；删除其中一个不让另一个成为其调用目标。
      for (const auto* member : name->second) {
        const auto alternative = static_cast<DeclarationKind>(member->kind);
        if ((kind != DeclarationKind::Getter && kind != DeclarationKind::Setter) ||
            (alternative != DeclarationKind::Getter && alternative != DeclarationKind::Setter)) {
          return member;
        }
      }
      return nullptr;
    }
    const auto type = classes_.find(owner);
    owner = type == classes_.end() ? DeclarationId{} : type->second->base_class;
  }
  return nullptr;
}

std::expected<void, NativeContractViolation> RetainedContracts::CheckBinding(
    const NativeFunctionDescription& function,
    const NativeBindingDependency& binding,
    const DeclarationIndex& source) const {
  const auto& original = *source.at(binding.target);
  const auto parent = source.find(original.owner);
  // 泛型应用的 owner 是模板声明；名字绑定检查模板在接收者中的归属，具体代码仍按应用身份选择。
  const auto& declaration =
      parent != source.end() && parent->second->kind == original.kind ? *parent->second : original;
  const auto target = declarations_.find(binding.target);
  const auto receiver = binding.receiver == DeclarationId{} ? declaration.owner : binding.receiver;
  const auto* member = FindMember(receiver, declaration);
  if (target == declarations_.end()) {
    if (previous_declarations_.contains(binding.target) && member != nullptr &&
        member->kind != declaration.kind) {
      return std::unexpected(
          Violation(function, binding.target,
                    "would change a retained name binding kind; Reload is required"));
    }
    return {};
  }
  if (member != nullptr && member->identity != declaration.identity) {
    return std::unexpected(Violation(
        function, binding.target,
        "would bind a retained member access to another declaration; Reload is required"));
  }
  if (static_cast<DeclarationKind>(declaration.kind) == DeclarationKind::Selector) {
    return {};
  }
  const auto implementation = functions_.find(binding.target);
  if (implementation == functions_.end() && target->second->definition != DeclarationId{}) {
    auto error = Violation(function, binding.target,
                           "retained generic target needs compilation in the candidate");
    error.status = NativeContractStatus::NeedsCompilation;
    return std::unexpected(std::move(error));
  }
  if (implementation != functions_.end() &&
      (implementation->second->flags & kNativePrivateFunction) != 0 &&
      function.dependencies->lexical_class != declaration.owner) {
    return std::unexpected(Violation(
        function, binding.target,
        "would access a private function outside its declaring class; Reload is required"));
  }
  return {};
}

std::expected<void, NativeContractViolation> RetainedContracts::CheckField(
    const NativeFunctionDescription& function,
    DeclarationId field) const {
  const auto found = fields_.find(field);
  if (found == fields_.end()) {
    return std::unexpected(
        Violation(function, field, "still accesses a deleted field; Reload is required"));
  }
  if ((found->second->flags & kNativePrivateField) != 0 &&
      function.dependencies->lexical_class != declarations_.at(field)->owner) {
    return std::unexpected(
        Violation(function, field,
                  "would access a private field outside its declaring class; Reload is required"));
  }
  return {};
}

std::expected<void, NativeContractViolation> RetainedContracts::CheckGlobal(
    const NativeFunctionDescription& function,
    DeclarationId global) const {
  const auto found = globals_.find(global);
  if (found == globals_.end()) {
    return std::unexpected(
        Violation(function, global, "still accesses a deleted global; Reload is required"));
  }
  if ((found->second & static_cast<std::uint32_t>(GlobalFlag::Private)) != 0 &&
      function.dependencies->lexical_class != declarations_.at(global)->owner) {
    return std::unexpected(Violation(
        function, global,
        "would access a private static field outside its declaring class; Reload is required"));
  }
  return {};
}

std::expected<void, NativeContractViolation> RetainedContracts::Check(
    const NativeFunctionDescription& function,
    const DeclarationIndex& source,
    const DependencySelection& selection) const {
  const auto* dependencies = function.dependencies;
  std::size_t index = 0;
  for (const auto field : dependencies->fields.values()) {
    if (!selection.CanReach(DependencyGroup::Field, index++)) {
      continue;
    }
    if (const auto checked = CheckField(function, field); !checked) {
      return checked;
    }
  }
  index = 0;
  for (const auto global : dependencies->globals.values()) {
    if (!selection.CanReach(DependencyGroup::Global, index++)) {
      continue;
    }
    if (const auto checked = CheckGlobal(function, global); !checked) {
      return checked;
    }
  }
  index = 0;
  for (const auto& construction : dependencies->constructions.values()) {
    if (!selection.CanReach(DependencyGroup::Construction, index++)) {
      continue;
    }
    if (const auto checked = CheckConstruction(function, construction); !checked) {
      return checked;
    }
  }
  index = 0;
  for (const auto& binding : dependencies->bindings.values()) {
    if (!selection.CanReach(DependencyGroup::Binding, index++)) {
      continue;
    }
    if (const auto checked = CheckBinding(function, binding, source); !checked) {
      return checked;
    }
  }
  return {};
}

void AppendVersions(const NativeRetainedFunction& reference,
                    const NativeFunctionDependencies& dependencies,
                    const DependencySelection& selection,
                    std::vector<NativeRetainedFunction>& pending) {
  std::size_t index = 0;
  for (const auto version : dependencies.versions.values()) {
    if (selection.CanReach(DependencyGroup::Version, index++)) {
      pending.push_back({.image = reference.image, .function = version});
    }
  }
}

void AppendDeclarations(const NativeFunctionDependencies& dependencies,
                        const DependencySelection& selection,
                        absl::flat_hash_set<DeclarationId>& declarations) {
  const auto append = [&](NativeTable<DeclarationId> table) {
    const auto values = table.values();
    declarations.insert(values.begin(), values.end());
  };
  const auto append_reachable = [&](NativeTable<DeclarationId> table, DependencyGroup group) {
    std::size_t index = 0;
    for (const auto value : table.values()) {
      if (selection.CanReach(group, index++)) {
        declarations.insert(value);
      }
    }
  };
  append_reachable(dependencies.fields, DependencyGroup::Field);
  append_reachable(dependencies.globals, DependencyGroup::Global);
  append(dependencies.types);
  std::size_t index = 0;
  for (const auto& binding : dependencies.bindings.values()) {
    if (selection.CanReach(DependencyGroup::Binding, index++)) {
      declarations.insert(binding.target);
      declarations.insert(binding.receiver);
    }
  }
  index = 0;
  for (const auto& construction : dependencies.constructions.values()) {
    if (selection.CanReach(DependencyGroup::Construction, index++)) {
      declarations.insert(construction.type);
      append(construction.supplied);
    }
  }
}

}  // namespace

std::expected<void, NativeContractViolation> CheckRetainedNativeContracts(
    std::span<const NativeRetainedFunction> retained,
    const NativeImage* previous,
    const NativeImage& candidate) {
  const RetainedContracts contracts(previous, candidate);
  std::vector<NativeRetainedFunction> pending(retained.begin(), retained.end());
  absl::flat_hash_set<
      std::tuple<const NativeImage*, std::uint32_t, std::uint32_t, NativeDependencyPhase>>
      visited;
  absl::flat_hash_map<const NativeImage*, DeclarationIndex> declarations;
  while (!pending.empty()) {
    const auto reference = pending.back();
    pending.pop_back();
    if (reference.image == &candidate ||
        !visited
             .emplace(reference.image, reference.function, reference.instruction.value_or(0),
                      reference.phase)
             .second) {
      continue;
    }
    const auto& image = *reference.image;
    const auto [entry, inserted] = declarations.try_emplace(&image);
    if (inserted) {
      for (const auto& declaration : image.declarations.values()) {
        entry->second.emplace(declaration.identity, &declaration);
      }
    }
    const auto& function = image.functions.values().subspan(reference.function).front();
    if (const auto* dependencies = function.dependencies) {
      const DependencySelection selection(*dependencies, reference.instruction.value_or(0),
                                          reference.phase);
      if (const auto checked = contracts.Check(function, entry->second, selection); !checked) {
        return checked;
      }
      AppendVersions(reference, *dependencies, selection, pending);
    }
  }
  return {};
}

std::vector<DeclarationId> RetainedNativeDeclarations(
    std::span<const NativeRetainedFunction> functions) {
  std::vector<NativeRetainedFunction> pending(functions.begin(), functions.end());
  absl::flat_hash_set<
      std::tuple<const NativeImage*, std::uint32_t, std::uint32_t, NativeDependencyPhase>>
      visited;
  absl::flat_hash_set<DeclarationId> declarations;
  while (!pending.empty()) {
    const auto reference = pending.back();
    pending.pop_back();
    if (!visited
             .emplace(reference.image, reference.function, reference.instruction.value_or(0),
                      reference.phase)
             .second) {
      continue;
    }
    const auto& function = reference.image->functions.values().subspan(reference.function).front();
    declarations.insert(function.identity.declaration);
    if (const auto* dependencies = function.dependencies) {
      const DependencySelection selection(*dependencies, reference.instruction.value_or(0),
                                          reference.phase);
      AppendDeclarations(*dependencies, selection, declarations);
      AppendVersions(reference, *dependencies, selection, pending);
    }
  }
  declarations.erase(DeclarationId{});
  return {declarations.begin(), declarations.end()};
}

std::expected<void, NativeContractViolation> CheckRetainedNativeTypes(
    std::span<const DeclarationId> declarations,
    const NativeImage& candidate) {
  absl::flat_hash_set<DeclarationId> classes;
  for (const auto& type : candidate.classes.values()) {
    classes.insert(type.declaration);
  }
  DeclarationIndex descriptions;
  for (const auto& declaration : candidate.declarations.values()) {
    descriptions.emplace(declaration.identity, &declaration);
  }
  for (const auto identity : declarations) {
    const auto found = descriptions.find(identity);
    if (found == descriptions.end()) {
      // 真正删除由名义契约及存留字段/调用检查报告；此处只区分特化裁剪。
      continue;
    }
    const auto& description = *found->second;
    if (description.kind == static_cast<std::uint32_t>(DeclarationKind::Class) &&
        description.definition != DeclarationId{} && !classes.contains(identity)) {
      return std::unexpected(NativeContractViolation{
          .declaration = identity,
          .message = "retained generic type needs compilation in the candidate",
          .status = NativeContractStatus::NeedsCompilation,
      });
    }
  }
  return {};
}

}  // namespace vela::runtime
