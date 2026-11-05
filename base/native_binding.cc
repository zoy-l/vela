#include "base/native_binding.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/fingerprint.h"

namespace vela {

std::string_view NativeMemberKindName(NativeMemberKind kind) {
  switch (kind) {
    case NativeMemberKind::Constructor:
      return "constructor";
    case NativeMemberKind::Method:
      return "method";
    case NativeMemberKind::Static:
      return "static";
    case NativeMemberKind::Getter:
      return "get";
    case NativeMemberKind::Setter:
      return "set";
  }
  std::unreachable();
}

std::string NativeMemberKey(std::string_view type, NativeMemberKind kind, std::string_view name) {
  std::string key(type);
  key += "::";
  key += NativeMemberKindName(kind);
  if (kind != NativeMemberKind::Constructor) {
    key += ':';
    key += name;
  }
  return key;
}

Fingerprint NativeClassContract(std::vector<NativeContractMember> members) {
  std::ranges::sort(members, {}, &NativeContractMember::key);
  std::string text = "vela.native-class/2;";
  const auto append = [&text](std::string_view value) {
    text += std::to_string(value.size()) + ':';
    text += value;
  };
  for (const auto& member : members) {
    append(member.key);
    append(std::to_string(member.parameters.size()));
    for (const auto& parameter : member.parameters) {
      append(parameter.name);
      append(parameter.type);
    }
    append(member.result);
  }
  return ContentFingerprint(text);
}

}  // namespace vela
