#include "runtime/isolate/member_table.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "base/member.h"
#include "runtime/heap/heap.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {

MemberTable::MemberTable(std::span<const TypeConstraint> types,
                         std::vector<MemberDefinition> members)
    : members_(std::move(members)) {
  for (std::size_t index = 0; index < types.size(); ++index) {
    const auto& type = types[index];
    if (!type.nullable && !type.dynamic && !type.callable) {
      receiver_indices_.try_emplace(Key(type), static_cast<std::uint32_t>(index));
    }
  }
  for (std::size_t index = 0; index < members_.size(); ++index) {
    const auto& member = members_[index];
    member_indices_.try_emplace(std::pair(member.receiver_type, std::string_view(member.name)),
                                index);
    const auto& type = types[member.receiver_type];
    if (type.nominal != nullptr && !type.nullable && !type.dynamic && !type.callable &&
        receiver_indices_.at(Key(type)) == member.receiver_type) {
      nominal_members_.try_emplace(std::pair(type.nominal, std::string_view(member.name)), index);
    }
  }
}

MemberTable::ReceiverKey MemberTable::Key(const TypeConstraint& type) {
  return {type.kind,       type.object_kind, type.nominal,  type.array,
          type.collection, type.signature,   type.interface};
}

const MemberDefinition* MemberTable::Find(Value receiver, std::string_view name) const {
  if (receiver.IsObject()) {
    if (const auto* layout = Heap::ClassOf(*receiver.AsObject())) {
      const auto found = nominal_members_.find(std::pair(&layout->nominal_type(), name));
      return found == nominal_members_.end() ? nullptr : &members_[found->second];
    }
  }
  return Find(Heap::TypeOf(receiver), name);
}

const MemberDefinition* MemberTable::Find(const TypeConstraint& receiver,
                                          std::string_view name) const {
  const auto key = Key(receiver);
  const auto type = receiver_indices_.find(key);
  if (type == receiver_indices_.end()) {
    return nullptr;
  }
  const auto member = member_indices_.find(std::pair(type->second, name));
  return member == member_indices_.end() ? nullptr : &members_[member->second];
}

Value MemberTable::ReadPublicField(Value receiver, std::string_view name) const {
  const auto* member = Find(receiver, name);
  if (member == nullptr || member->kind != MemberKind::Field) {
    return Value::Null();
  }
  return Heap::LoadField(*receiver.AsObject(), member->target);
}

}  // namespace vela::runtime
