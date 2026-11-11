#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>

namespace vela {

struct DeclarationId final {
  std::uint64_t value = std::numeric_limits<std::uint64_t>::max();
  bool operator==(const DeclarationId&) const = default;
  template <class H>
  friend H AbslHashValue(H state, DeclarationId identity) {
    return H::combine(std::move(state), identity.value);
  }
};

// 声明目录由生产端按 identity 排序，消费者共享同一存在性查询。
template <class Entry>
[[nodiscard]] const Entry* FindDeclaration(std::span<const Entry> declarations,
                                           DeclarationId identity) noexcept {
  const auto found = std::ranges::lower_bound(
      declarations, identity.value, {}, [](const auto& entry) { return entry.identity.value; });
  return found != declarations.end() && found->identity == identity ? &*found : nullptr;
}

enum class DeclarationKind : std::uint8_t {
  Module,
  Function,
  Class,
  Interface,
  Enum,
  Alias,
  Method,
  StaticMethod,
  Getter,
  Setter,
  Constructor,
  Field,
  StaticField,
  Global,
  Selector,
  Initializer,
  Property,
  EnumMember,
};

enum class GlobalFlag : std::uint8_t { Constant = 1, Private = 2 };

// local 为零表示命名实现；非零表示同一外层版本内的匿名函数。
struct FunctionIdentity final {
  DeclarationId declaration;
  std::uint64_t revision = 0;
  std::uint32_t local = 0;
  bool operator==(const FunctionIdentity&) const = default;
};

}  // namespace vela
