#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "base/declaration.h"
#include "compiler/frontend/syntax/ast.h"

namespace vela::frontend {

struct TypeIdentity final {
  enum class Kind : std::uint8_t { Builtin, Declared, Array, Nullable, Function, Generic };
  Kind kind = Kind::Builtin;
  DeclarationId declaration;
  std::string builtin;
  std::vector<TypeIdentity> arguments;
  bool operator==(const TypeIdentity&) const = default;
  template <class H>
  friend H AbslHashValue(H state, const TypeIdentity& type) {
    return H::combine(std::move(state), type.kind, type.declaration, type.builtin, type.arguments);
  }
};

struct DeclarationKey final {
  DeclarationId owner;
  DeclarationKind kind = DeclarationKind::Function;
  std::string name;
  std::vector<TypeIdentity> arguments;
  bool operator==(const DeclarationKey&) const = default;
  template <class H>
  friend H AbslHashValue(H state, const DeclarationKey& key) {
    return H::combine(std::move(state), key.owner, key.kind, key.name, key.arguments);
  }
};

struct DeclarationRecord final {
  DeclarationKey key;
  DeclarationId definition;
  std::uint64_t seen = 0;
  bool retired = false;
};

// 泛型声明的契约文本，每个参数一项：参数引用记为所属声明中的序号，名义类型记为模块 URI 与声明名。
// 参数重命名和导入前缀变化不改变结果；参数数量或语义上界变化会改变结果。
[[nodiscard]] std::vector<std::string> DescribeTypeParameterContracts(
    std::span<const TypeParameter> parameters,
    const absl::flat_hash_map<std::string, DeclarationOrigin>& origins);
[[nodiscard]] std::string DescribeTypeContract(
    const TypeName& type,
    const std::string& parameter_owner,
    const absl::flat_hash_map<std::string, DeclarationOrigin>& origins);

// 每次编译操作工作副本。删除只退休身份，后续同名声明重新分配；失败时直接丢弃副本。
class DeclarationIds final {
 public:
  void BeginUpdate() noexcept;
  [[nodiscard]] DeclarationId Get(DeclarationKey key, DeclarationId definition = {});
  void FinishUpdate();
  [[nodiscard]] std::span<const DeclarationRecord> records() const noexcept { return records_; }

 private:
  std::uint64_t update_ = 0;
  std::vector<DeclarationRecord> records_;
  absl::flat_hash_map<DeclarationKey, DeclarationId> active_;
};

}  // namespace vela::frontend
