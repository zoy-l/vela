#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/frontend/typing/type_checker.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

namespace vela::frontend {

TypeChecker::Refinements::Refinements(Refinements&& other) noexcept
    : facts_(std::exchange(other.facts_, {})),
      undo_(std::exchange(other.undo_, {})),
      conditions_(std::exchange(other.conditions_, {})),
      dependents_(std::exchange(other.dependents_, {})),
      condition_undo_(std::exchange(other.condition_undo_, {})),
      branch_depth_(std::exchange(other.branch_depth_, 0)) {}

TypeChecker::Refinements& TypeChecker::Refinements::operator=(Refinements&& other) noexcept {
  facts_ = std::exchange(other.facts_, {});
  undo_ = std::exchange(other.undo_, {});
  conditions_ = std::exchange(other.conditions_, {});
  dependents_ = std::exchange(other.dependents_, {});
  condition_undo_ = std::exchange(other.condition_undo_, {});
  branch_depth_ = std::exchange(other.branch_depth_, 0);
  return *this;
}

const TypeChecker::Type* TypeChecker::Refinements::Find(std::uint32_t binding) const {
  const auto found = facts_.find(binding);
  return found == facts_.end() ? nullptr : &found->second;
}

bool TypeChecker::Refinements::IsEmpty() const {
  return facts_.empty() && conditions_.empty();
}

TypeChecker::Refinements TypeChecker::Refinements::Snapshot() const {
  Refinements result;
  result.facts_ = facts_;
  result.conditions_ = conditions_;
  result.dependents_ = dependents_;
  return result;
}

TypeChecker::ConditionFacts TypeChecker::Refinements::FindCondition(std::uint32_t binding) const {
  const auto found = conditions_.find(binding);
  return found == conditions_.end() ? nullptr : found->second;
}

void TypeChecker::Refinements::AssignCondition(std::uint32_t binding, const ConditionFacts& facts) {
  enum class Update : std::uint8_t { Insert, Erase };
  const auto update_dependencies = [&](const ConditionFacts& condition, Update update) {
    if (!condition) {
      return;
    }
    for (const auto* branch : {&condition->when_true, &condition->when_false}) {
      for (const auto& [dependency, type] : *branch) {
        if (update == Update::Insert) {
          dependents_[dependency].insert(binding);
        } else if (const auto found = dependents_.find(dependency); found != dependents_.end()) {
          found->second.erase(binding);
          if (found->second.empty()) {
            dependents_.erase(found);
          }
        }
      }
    }
  };
  update_dependencies(FindCondition(binding), Update::Erase);
  if (facts) {
    conditions_.insert_or_assign(binding, facts);
  } else {
    conditions_.erase(binding);
  }
  update_dependencies(facts, Update::Insert);
}

void TypeChecker::Refinements::SetCondition(std::uint32_t binding, const ConditionFacts& facts) {
  const auto previous = FindCondition(binding);
  if (previous == facts) {
    return;
  }
  if (branch_depth_ != 0) {
    condition_undo_.emplace_back(binding, previous);
  }
  AssignCondition(binding, facts);
}

void TypeChecker::Refinements::Invalidate(std::uint32_t binding) {
  Erase(binding);
  SetCondition(binding, nullptr);
  if (const auto found = dependents_.find(binding); found != dependents_.end()) {
    // SetCondition 会更新反向索引，先复制这次需要撤销的别名。
    const auto aliases = found->second;
    for (const auto alias : aliases) {
      SetCondition(alias, nullptr);
    }
  }
}

void TypeChecker::Refinements::Save(std::uint32_t binding, const Type* previous) {
  if (branch_depth_ != 0) {
    undo_.push_back({
        .binding = binding,
        .previous = previous == nullptr ? std::nullopt : std::optional(*previous),
    });
  }
}

void TypeChecker::Refinements::Set(std::uint32_t binding, const Type& type) {
  const auto* previous = Find(binding);
  if (previous != nullptr && IsSameType(*previous, type)) {
    return;
  }
  Save(binding, previous);
  facts_.insert_or_assign(binding, type);
}

void TypeChecker::Refinements::Erase(std::uint32_t binding) {
  const auto found = facts_.find(binding);
  if (found == facts_.end()) {
    return;
  }
  Save(binding, &found->second);
  facts_.erase(found);
}

void TypeChecker::Refinements::Clear() {
  if (branch_depth_ != 0) {
    for (const auto& [binding, type] : facts_) {
      Save(binding, &type);
    }
  }
  facts_.clear();
  if (branch_depth_ != 0) {
    for (const auto& [binding, facts] : conditions_) {
      condition_undo_.emplace_back(binding, facts);
    }
  }
  conditions_.clear();
  dependents_.clear();
}

void TypeChecker::Refinements::Erase(const absl::flat_hash_set<std::uint32_t>& bindings) {
  for (const auto binding : bindings) {
    Invalidate(binding);
  }
}

TypeChecker::Refinements::Checkpoint TypeChecker::Refinements::BeginBranch() {
  ++branch_depth_;
  return {.offset = undo_.size(), .condition_offset = condition_undo_.size()};
}

void TypeChecker::Refinements::Discard(Checkpoint checkpoint) {
  // 分支按嵌套顺序退出；恢复直接写入事实表，不把撤销动作再次记入日志。
  while (undo_.size() > checkpoint.offset) {
    auto change = undo_.back();
    undo_.pop_back();
    if (change.previous) {
      facts_.insert_or_assign(change.binding, *change.previous);
    } else {
      facts_.erase(change.binding);
    }
  }
  while (condition_undo_.size() > checkpoint.condition_offset) {
    auto [binding, facts] = std::move(condition_undo_.back());
    condition_undo_.pop_back();
    AssignCondition(binding, facts);
  }
  --branch_depth_;
}

TypeChecker::RefinementDelta TypeChecker::Refinements::Capture(Checkpoint checkpoint) {
  RefinementDelta changes;
  for (std::size_t index = checkpoint.offset; index < undo_.size(); ++index) {
    const auto binding = undo_.at(index).binding;
    if (!changes.types.contains(binding)) {
      const auto* type = Find(binding);
      changes.types.emplace(binding, type == nullptr ? std::nullopt : std::optional(*type));
    }
  }
  for (std::size_t index = checkpoint.condition_offset; index < condition_undo_.size(); ++index) {
    const auto binding = condition_undo_[index].first;
    changes.conditions.try_emplace(binding, FindCondition(binding));
  }
  Discard(checkpoint);
  return changes;
}

void TypeChecker::Refinements::Apply(const RefinementDelta& changes) {
  for (const auto& [binding, type] : changes.types) {
    if (type) {
      Set(binding, *type);
    } else {
      Erase(binding);
    }
  }
  for (const auto& [binding, facts] : changes.conditions) {
    SetCondition(binding, facts);
  }
}

void TypeChecker::Refinements::MergeAlternatives(RefinementDelta& first,
                                                const RefinementDelta& second,
                                                AlternativeCompletions completes) {
  if (!completes.first) {
    Apply(second);
    return;
  }
  if (completes.second) {
    Intersect(first, second);
  }
  Apply(first);
}

void TypeChecker::Refinements::Intersect(RefinementDelta& target,
                                         const RefinementDelta& other) const {
  // 两个增量以当前事实表为共同基线；没有修改的绑定直接读取基线。
  for (auto& [binding, type] : target.types) {
    const auto found = other.types.find(binding);
    const TypeDescriptor* right = nullptr;
    if (found == other.types.end()) {
      right = Find(binding);
    } else if (found->second) {
      right = &*found->second;
    }
    if (!type || right == nullptr || !IsSameType(*type, *right)) {
      type.reset();
    }
  }
  for (const auto& [binding, type] : other.types) {
    if (target.types.contains(binding)) {
      continue;
    }
    const auto* left = Find(binding);
    if (!type || left == nullptr || !IsSameType(*left, *type)) {
      target.types.emplace(binding, std::nullopt);
    }
  }
  for (auto& [binding, facts] : target.conditions) {
    const auto found = other.conditions.find(binding);
    const auto right = found == other.conditions.end() ? FindCondition(binding) : found->second;
    if (facts != right) {
      facts.reset();
    }
  }
  for (const auto& [binding, facts] : other.conditions) {
    if (!target.conditions.contains(binding) && facts != FindCondition(binding)) {
      target.conditions.emplace(binding, nullptr);
    }
  }
}

}  // namespace vela::frontend
