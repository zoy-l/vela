#include "compiler/frontend/modules/declaration_ids.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "base/declaration.h"
#include "compiler/frontend/syntax/ast.h"

namespace vela::frontend {
DeclarationIds DeclarationIds::FromRecords(std::vector<DeclarationRecord> records) {
  DeclarationIds result;
  result.records_ = std::move(records);
  for (std::size_t index = 0; index < result.records_.size(); ++index) {
    const auto& record = result.records_[index];
    result.update_ = std::max(result.update_, record.seen);
    if (!record.retired) {
      result.active_.emplace(record.key, DeclarationId{index});
    }
  }
  return result;
}
namespace {

std::string ContractText(const TypeName& type,
                         const std::string& owner,
                         const absl::flat_hash_map<std::string, DeclarationOrigin>& origins) {
  using Kind = TypeName::Kind;
  switch (type.kind) {
    case Kind::Inferred:
      return {};
    case Kind::Parameter:
      // 方法上界可引用所属 class 的参数；两层作用域分别以前缀区分，不依赖参数名。
      return (type.parameter.owner == owner ? "#" : "^#") + std::to_string(type.parameter.index);
    case Kind::Named:
    case Kind::Generic: {
      const auto origin = origins.find(type.name);
      auto text = origin == origins.end() ? type.name
                                          : origin->second.module_uri + "::" + origin->second.name;
      if (type.kind == Kind::Generic) {
        text += '<';
        for (std::size_t index = 0; index < type.arguments.size(); ++index) {
          text += (index == 0 ? "" : ", ") + ContractText(type.arguments.at(index), owner, origins);
        }
        text += '>';
      }
      return text;
    }
    case Kind::Array:
    case Kind::Nullable:
      return '(' + ContractText(type.arguments.front(), owner, origins) +
             (type.kind == Kind::Array ? ")[]" : ")?");
    case Kind::Function: {
      std::string text = "(";
      for (std::size_t index = 0; index + 1 < type.arguments.size(); ++index) {
        text += (index == 0 ? "" : ", ") + ContractText(type.arguments.at(index), owner, origins);
      }
      return text + ") => " + ContractText(type.arguments.back(), owner, origins);
    }
  }
  std::unreachable();
}

}  // namespace

std::vector<std::string> DescribeTypeParameterContracts(
    std::span<const TypeParameter> parameters,
    const absl::flat_hash_map<std::string, DeclarationOrigin>& origins) {
  std::vector<std::string> result;
  result.reserve(parameters.size());
  for (const auto& parameter : parameters) {
    result.push_back(parameter.bound ? "extends " + ContractText(*parameter.bound,
                                                                 parameter.identity.owner, origins)
                                     : std::string());
  }
  return result;
}

std::string DescribeTypeContract(
    const TypeName& type,
    const std::string& parameter_owner,
    const absl::flat_hash_map<std::string, DeclarationOrigin>& origins) {
  return ContractText(type, parameter_owner, origins);
}

void DeclarationIds::BeginUpdate() noexcept {
  ++update_;
}

DeclarationId DeclarationIds::Get(DeclarationKey key, DeclarationId definition) {
  const auto [found, inserted] =
      active_.try_emplace(key, DeclarationId{static_cast<std::uint64_t>(records_.size())});
  const auto identity = found->second;
  if (inserted) {
    records_.push_back({.key = std::move(key), .definition = definition, .seen = update_});
  } else {
    records_[static_cast<std::size_t>(identity.value)].seen = update_;
  }
  return identity;
}

std::optional<DeclarationId> DeclarationIds::Find(const DeclarationKey& key) const {
  const auto found = active_.find(key);
  return found == active_.end() ? std::nullopt : std::optional(found->second);
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
    const auto owner = record.key.owner;
    const auto owner_active = is_active(owner);
    // 方法选择器随所属成员保留；不可达泛型应用无需重新生成方法体或派发表。
    bool method_selector = false;
    if (record.key.kind == DeclarationKind::Selector && owner_active) {
      const auto kind = records_.at(static_cast<std::size_t>(owner.value)).key.kind;
      method_selector = kind == DeclarationKind::Method || kind == DeclarationKind::Getter ||
                        kind == DeclarationKind::Setter;
    }
    auto retained = owner_active && (method_selector || is_active(record.definition));
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
