#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/container/node_hash_map.h>

#include "compiler/frontend/ast.h"
#include "compiler/frontend/diagnostic.h"

namespace vela::frontend {

// 内建 API 投影载荷与回调契约时使用的推导变量；身份位于保留命名空间，不与源码参数冲突。
[[nodiscard]] TypeParameter IntrinsicTypeParameter(std::string_view name);
[[nodiscard]] TypeName IntrinsicTypeVariable(std::string_view name, SourceLocation location);

// 求解器借用检查器的静态关系和声明投影，不复制名义关系或建立独立的类型规则。
class InferenceTypeRelations {
 public:
  struct Projection final {
    const TypeName* subtype = nullptr;
    const TypeName* supertype = nullptr;
  };
  InferenceTypeRelations(const InferenceTypeRelations&) = delete;
  InferenceTypeRelations& operator=(const InferenceTypeRelations&) = delete;
  InferenceTypeRelations(InferenceTypeRelations&&) = delete;
  InferenceTypeRelations& operator=(InferenceTypeRelations&&) = delete;
  virtual std::vector<TypeName> ProjectSupertypes(const Projection& projection) = 0;
  [[nodiscard]] virtual bool IsStaticSubtype(const TypeName& subtype,
                                             const TypeName& supertype) = 0;
  [[nodiscard]] virtual bool CanBindType(const TypeName& destination, const TypeName& source) = 0;
  [[nodiscard]] virtual bool IsInvariantType(const TypeName& type) const = 0;

 protected:
  InferenceTypeRelations() = default;
  ~InferenceTypeRelations() = default;
};

// 推导变量只存在于一次调用的约束域中，不会进入 SemanticModel 或后端。
// 模式使用结构化类型并按参数身份识别变量；来源类型必须由类型检查器提供，不能从表达式文本猜测。
class TypeInference final {
 public:
  // Callback 表示 intrinsic 的逐参数受检查转换契约；普通函数实参使用 Argument。
  enum class Evidence : std::uint8_t { Context, Callback, Argument, Explicit, Bound };
  struct Failure final {
    enum class Kind : std::uint8_t { Unresolved, Conflicting, Bound, Relation };
    Kind kind;
    std::string parameter;
    TypeName actual;
    TypeName required;
  };
  class Solution final {
   public:
    [[nodiscard]] TypeName Substitute(TypeName pattern) const;

   private:
    friend class TypeInference;
    TypeSubstitutions variables_;
  };

  TypeInference(std::span<const TypeParameter> parameters, InferenceTypeRelations& relations);
  [[nodiscard]] static std::optional<TypeName> CommonType(const TypeName& left,
                                                          const TypeName& right);
  void Constrain(const TypeName& pattern, const TypeName& actual, Evidence evidence);
  [[nodiscard]] std::optional<TypeName> Substitute(const TypeName& pattern) const;
  [[nodiscard]] bool CanResolveFrom(std::span<const TypeName> inputs) const;
  [[nodiscard]] std::expected<Solution, Failure> Solve() const;

 private:
  enum class Relation : std::uint8_t { Input, Context, Equal, Bound, Callback, Conversion };
  struct Binding final {
    TypeName type;
    Relation relation = Relation::Input;
  };
  struct Candidate final {
    TypeName type;
    bool from_bound = false;
    bool allows_subtype = false;
    bool valid = true;
    std::size_t checked_bindings = 0;
  };
  struct Variable final {
    std::optional<TypeName> type;
    std::vector<Binding> bindings;
    absl::flat_hash_set<std::tuple<std::string, Evidence, Relation>> binding_keys;
    absl::flat_hash_map<std::string, Candidate> candidates;
    bool fixed = false;
  };
  struct Constraint final {
    TypeName pattern;
    TypeName actual;
    Evidence evidence = Evidence::Context;
  };
  struct BoundWork final {
    TypeParameterId parameter;
    TypeName type;
  };
  void Bind(const TypeParameter& parameter,
            const TypeName& type,
            Evidence evidence,
            Relation relation);
  void AddCandidate(const TypeParameterId& parameter, const TypeName& type, Evidence evidence);
  void Match(const TypeName& pattern, const TypeName& actual, Evidence evidence, Relation relation);
  [[nodiscard]] static Relation EvidenceRelation(Evidence evidence);
  [[nodiscard]] Relation ArgumentRelation(const TypeName& pattern,
                                          Relation relation,
                                          std::size_t index) const;
  void Propagate();
  void SelectCandidates();
  [[nodiscard]] bool IsValidCandidate(const Variable& variable, Candidate& candidate) const;
  [[nodiscard]] bool IsValidBinding(const Candidate& candidate, const Binding& binding) const;
  [[nodiscard]] bool IsStaticSubtype(const TypeName& subtype, const TypeName& supertype) const;
  [[nodiscard]] bool CanBindType(const TypeName& destination, const TypeName& source) const;
  [[nodiscard]] bool IsValidConstraint(const TypeName& pattern,
                                       const TypeName& actual,
                                       Relation relation) const;
  [[nodiscard]] std::span<const TypeName> SupertypeProjections(const TypeName& source,
                                                               const TypeName& constructor) const;
  [[nodiscard]] bool IsMissingBoundProjection(const TypeName& actual, const TypeName& bound) const;
  [[nodiscard]] static bool HasUnspecifiedType(const TypeName& type);
  [[nodiscard]] static bool IsSame(const TypeName& left, const TypeName& right);
  [[nodiscard]] const TypeParameter* FindParameter(const TypeName& pattern) const;
  // 检查器覆盖一次完整类型检查；每个局部推导域在其内部创建和销毁。
  InferenceTypeRelations* relations_;
  std::vector<TypeParameter> parameters_;
  absl::flat_hash_map<TypeParameterId, Variable> variables_;
  absl::flat_hash_map<TypeParameterId, std::size_t> parameter_indices_;
  std::vector<Constraint> constraints_;
  std::vector<BoundWork> bound_work_;
  absl::flat_hash_set<std::pair<TypeParameterId, std::string>> propagated_bounds_;
  mutable absl::node_hash_map<std::pair<std::string, std::string>, std::vector<TypeName>>
      projections_;
  mutable absl::flat_hash_map<std::pair<std::string, std::string>, bool> static_relations_;
  mutable absl::flat_hash_map<std::pair<std::string, std::string>, bool> binding_relations_;
  std::vector<TypeParameterId> conflicts_;
};

}  // namespace vela::frontend
