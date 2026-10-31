#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "base/member.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {

// 类型表借用实例的稳定布局；成员描述由本表拥有，索引中的名称视图随其存活。
class MemberTable final {
 public:
  MemberTable() = default;
  MemberTable(std::span<const TypeConstraint> types, std::vector<MemberDefinition> members);
  ~MemberTable() = default;
  MemberTable(const MemberTable&) = delete;
  MemberTable& operator=(const MemberTable&) = delete;
  MemberTable(MemberTable&&) = default;
  MemberTable& operator=(MemberTable&&) = default;

  [[nodiscard]] const std::vector<MemberDefinition>& members() const noexcept { return members_; }
  [[nodiscard]] const MemberDefinition* Find(Value receiver, std::string_view name) const;
  [[nodiscard]] bool IsCanonicalReceiverType(std::uint32_t index) const;

 private:
  using ReceiverKey = std::tuple<ValueKind,
                                 ObjectKind,
                                 const NominalType*,
                                 const ArrayLayout*,
                                 const CollectionLayout*,
                                 const TypeSignature*>;
  [[nodiscard]] static ReceiverKey Key(const TypeConstraint& type);
  std::span<const TypeConstraint> types_;
  std::vector<MemberDefinition> members_;
  absl::flat_hash_map<ReceiverKey, std::uint32_t> receiver_indices_;
  absl::flat_hash_map<std::pair<std::uint32_t, std::string_view>, std::size_t> member_indices_;
};

}  // namespace vela::runtime
