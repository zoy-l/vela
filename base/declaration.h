#pragma once

#include <cstdint>
#include <limits>
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
};

enum class GlobalBinding : std::uint32_t { Mutable, Constant };

// local 为零表示命名实现；非零表示同一外层版本内的匿名函数。
struct FunctionIdentity final {
  DeclarationId declaration;
  std::uint64_t revision = 0;
  std::uint32_t local = 0;
  bool operator==(const FunctionIdentity&) const = default;
};

}  // namespace vela
