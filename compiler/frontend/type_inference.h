#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "compiler/frontend/ast.h"

namespace vela::frontend {

// 推导变量只存在于一次调用的约束域中，不会进入 SemanticModel 或后端。
// 模式使用结构化类型；来源类型必须由类型检查器提供，不能从表达式文本猜测。
class TypeInference final {
 public:
  enum class Evidence : std::uint8_t { Context, Callback, Argument, Explicit };
  class Solution final {
   public:
    [[nodiscard]] TypeName Substitute(TypeName pattern) const;

   private:
    friend class TypeInference;
    absl::flat_hash_map<std::string, TypeName> variables_;
  };

  explicit TypeInference(std::span<const std::string> parameters);
  [[nodiscard]] static std::optional<TypeName> CommonType(const TypeName& left,
                                                          const TypeName& right);
  void Constrain(const TypeName& pattern, const TypeName& actual, Evidence evidence);
  [[nodiscard]] std::optional<TypeName> Substitute(const TypeName& pattern) const;
  [[nodiscard]] std::vector<std::string> Unresolved() const;
  [[nodiscard]] bool CanResolveFrom(std::span<const TypeName> inputs) const;
  [[nodiscard]] std::optional<Solution> Solve() const;
  [[nodiscard]] const std::vector<std::string>& conflicts() const { return conflicts_; }

 private:
  struct Variable final {
    std::optional<TypeName> type;
    Evidence evidence = Evidence::Context;
  };
  void Bind(const std::string& name, const TypeName& type, Evidence evidence);
  std::vector<std::string> parameters_;
  absl::flat_hash_map<std::string, Variable> variables_;
  std::vector<std::string> conflicts_;
};

}  // namespace vela::frontend
