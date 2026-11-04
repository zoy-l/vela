#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "base/fingerprint.h"

namespace vela {

enum class NativeMemberKind : std::uint8_t { Constructor, Method, Static, Getter, Setter };

[[nodiscard]] constexpr bool HasNativeReceiver(NativeMemberKind kind) noexcept {
  return kind == NativeMemberKind::Method || kind == NativeMemberKind::Getter ||
         kind == NativeMemberKind::Setter;
}

[[nodiscard]] std::string_view NativeMemberKindName(NativeMemberKind kind);
[[nodiscard]] std::string NativeMemberKey(std::string_view type,
                                          NativeMemberKind kind,
                                          std::string_view name);

// 源码、bindgen 和 runtime 生产者共享的临时规范化输入；不作为另一份持久类型表。
struct NativeContractType final {
  std::string key;
  bool nullable = false;
};

struct NativeContractMember final {
  struct Parameter final {
    std::string name;
    NativeContractType type;
  };
  std::string key;
  std::vector<Parameter> parameters;
  NativeContractType result;
};

[[nodiscard]] Fingerprint NativeClassContract(std::vector<NativeContractMember> members);

}  // namespace vela
