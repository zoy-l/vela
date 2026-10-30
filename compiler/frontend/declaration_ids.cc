#include "compiler/frontend/declaration_ids.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>

#include <absl/container/flat_hash_map.h>

#include "base/declaration.h"

namespace vela::frontend {

void DeclarationIds::BeginUpdate() noexcept {
  ++update_;
}

DeclarationId DeclarationIds::Get(DeclarationKey key, DeclarationId definition) {
  const auto found = active_.find(key);
  if (found != active_.end()) {
    const auto identity = found->second;
    records_.at(static_cast<std::size_t>(identity.value)).seen = update_;
    return identity;
  }
  const DeclarationId identity{static_cast<std::uint64_t>(records_.size())};
  active_.emplace(key, identity);
  records_.push_back({.key = std::move(key), .definition = definition, .seen = update_});
  return identity;
}

void DeclarationIds::FinishUpdate() {
  const auto is_active = [&](DeclarationId identity) {
    return identity != DeclarationId{} &&
           !records_.at(static_cast<std::size_t>(identity.value)).retired;
  };
  const auto has_active_arguments = [&](const auto& self, const TypeIdentity& type) -> bool {
    if (type.declaration != DeclarationId{} && !is_active(type.declaration)) {
      return false;
    }
    return std::ranges::all_of(type.arguments,
                               [&](const auto& argument) { return self(self, argument); });
  };
  // Get 的依赖先于使用者分配：模块、模板、类型实参、具体化、成员、初始化器。
  // 顺序退休可在线性时间传播真实删除；不要求为不可达特化生成函数体。
  for (auto& record : records_) {
    if (record.retired) {
      continue;
    }
    if (record.seen == update_) {
      continue;
    }
    auto retained = is_active(record.definition) && is_active(record.key.owner);
    for (const auto& argument : record.key.arguments) {
      retained = retained && has_active_arguments(has_active_arguments, argument);
    }
    record.retired = !retained;
  }
  absl::erase_if(active_, [&](const auto& entry) {
    return records_.at(static_cast<std::size_t>(entry.second.value)).retired;
  });
}

}  // namespace vela::frontend
