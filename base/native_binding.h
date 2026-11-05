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

// 类型键由共享 layout 描述生成，包含可空性及全部嵌套契约。
struct NativeContractMember final {
  struct Parameter final {
    std::string name;
    std::string type;
  };
  std::string key;
  std::vector<Parameter> parameters;
  std::string result;
};

[[nodiscard]] Fingerprint NativeClassContract(std::vector<NativeContractMember> members);

}  // namespace vela
