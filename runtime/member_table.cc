#include "runtime/member_table.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "base/member.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/value.h"

namespace vela::runtime {

MemberTable::MemberTable(std::span<const TypeConstraint> types,
                         std::vector<MemberDefinition> members)
    : types_(types), members_(std::move(members)) {
  for (std::size_t index = 0; index < types_.size(); ++index) {
    const auto& type = types_.subspan(index).front();
    if (!type.nullable && !type.dynamic && type.interface == nullptr && !type.callable) {
      receiver_indices_.try_emplace(Key(type), static_cast<std::uint32_t>(index));
    }
  }
  for (std::size_t index = 0; index < members_.size(); ++index) {
    const auto& member = members_.at(index);
    member_indices_.try_emplace(std::pair(member.receiver_type, std::string_view(member.name)),
                                index);
  }
}

MemberTable::ReceiverKey MemberTable::Key(const TypeConstraint& type) {
  return {type.kind, type.object_kind, type.nominal, type.array, type.collection, type.signature};
}

bool MemberTable::IsCanonicalReceiverType(std::uint32_t index) const {
  if (index >= types_.size()) {
    return false;
  }
  const auto found = receiver_indices_.find(Key(types_.subspan(index).front()));
  return found != receiver_indices_.end() && found->second == index;
}

const MemberDefinition* MemberTable::Find(Value receiver, std::string_view name) const {
  const auto key = Key(Heap::TypeOf(receiver));
  const auto type = receiver_indices_.find(key);
  if (type == receiver_indices_.end()) {
    return nullptr;
  }
  const auto member = member_indices_.find(std::pair(type->second, name));
  return member == member_indices_.end() ? nullptr : &members_.at(member->second);
}

}  // namespace vela::runtime
