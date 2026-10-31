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
      branch_depth_(std::exchange(other.branch_depth_, 0)) {}

TypeChecker::Refinements& TypeChecker::Refinements::operator=(Refinements&& other) noexcept {
  facts_ = std::exchange(other.facts_, {});
  undo_ = std::exchange(other.undo_, {});
  branch_depth_ = std::exchange(other.branch_depth_, 0);
  return *this;
}

const TypeChecker::Type* TypeChecker::Refinements::Find(std::uint32_t binding) const {
  const auto found = facts_.find(binding);
  return found == facts_.end() ? nullptr : &found->second;
}

bool TypeChecker::Refinements::IsEmpty() const {
  return facts_.empty();
}

TypeChecker::Refinements TypeChecker::Refinements::Snapshot() const {
  Refinements result;
  result.facts_ = facts_;
  return result;
}

void TypeChecker::Refinements::Save(std::uint32_t binding, const Type* previous) {
  if (branch_depth_ != 0) {
    undo_.push_back({
        .binding = binding,
        .previous = previous == nullptr ? std::nullopt : std::optional(*previous),
    });
  }
}

void TypeChecker::Refinements::Set(std::uint32_t binding, Type type) {
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
}

void TypeChecker::Refinements::Erase(const absl::flat_hash_set<std::uint32_t>& bindings) {
  absl::erase_if(facts_, [&](const auto& fact) {
    if (!bindings.contains(fact.first)) {
      return false;
    }
    Save(fact.first, &fact.second);
    return true;
  });
}

TypeChecker::Refinements::Checkpoint TypeChecker::Refinements::BeginBranch() {
  ++branch_depth_;
  return {.offset = undo_.size()};
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
  --branch_depth_;
}

TypeChecker::RefinementDelta TypeChecker::Refinements::Capture(Checkpoint checkpoint) {
  RefinementDelta changes;
  for (std::size_t index = checkpoint.offset; index < undo_.size(); ++index) {
    const auto binding = undo_.at(index).binding;
    if (!changes.contains(binding)) {
      const auto* type = Find(binding);
      changes.emplace(binding, type == nullptr ? std::nullopt : std::optional(*type));
    }
  }
  Discard(checkpoint);
  return changes;
}

void TypeChecker::Refinements::Apply(const RefinementDelta& changes) {
  for (const auto& [binding, type] : changes) {
    if (type) {
      Set(binding, *type);
    } else {
      Erase(binding);
    }
  }
}

void TypeChecker::Refinements::Intersect(RefinementDelta& target,
                                         const RefinementDelta& other) const {
  // 两个增量以当前事实表为共同基线；没有修改的绑定直接读取基线。
  for (auto& [binding, type] : target) {
    const auto found = other.find(binding);
    const TypeDescriptor* right = nullptr;
    if (found == other.end()) {
      right = Find(binding);
    } else if (found->second) {
      right = &*found->second;
    }
    if (!type || right == nullptr || !IsSameType(*type, *right)) {
      type.reset();
    }
  }
  for (const auto& [binding, type] : other) {
    if (target.contains(binding)) {
      continue;
    }
    const auto* left = Find(binding);
    if (!type || left == nullptr || !IsSameType(*left, *type)) {
      target.emplace(binding, std::nullopt);
    }
  }
}

}  // namespace vela::frontend
