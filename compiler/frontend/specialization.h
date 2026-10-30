#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include "base/declaration.h"
#include "compiler/frontend/ast.h"
#include "compiler/frontend/ast_clone.h"
#include "compiler/frontend/declaration_ids.h"
#include "compiler/frontend/diagnostic.h"

namespace vela::frontend {

struct GenericSpecialization final {
  std::string name;
  std::vector<TypeName> arguments;
};

// 用当前模块绑定重建存留特化，不能复用旧导入别名产生的扁平名字。
[[nodiscard]] std::expected<std::vector<GenericSpecialization>, Diagnostics>
RetainedSpecializations(const Program& program,
                        const DeclarationIds& identities,
                        std::span<const DeclarationId> declarations);

// 语义检查期间拥有模板及新增特化；声明地址在 Publish 前保持稳定。
// 类型实参由类型检查器求解，本类只负责替换、规范化和唯一化具体声明。
class Specializer final : public AstCloneTransform {
 public:
  explicit Specializer(Program& program) : program_(program) {}
  ~Specializer() override = default;
  Specializer(const Specializer&) = delete;
  Specializer& operator=(const Specializer&) = delete;
  Specializer(Specializer&&) = delete;
  Specializer& operator=(Specializer&&) = delete;

  Diagnostics Initialize(std::span<const GenericSpecialization> retained);
  [[nodiscard]] const Function* FunctionTemplate(std::string_view name) const;
  [[nodiscard]] const Class* ClassTemplate(std::string_view name) const;
  [[nodiscard]] const Program::TypeAlias* AliasTemplate(std::string_view name) const;
  [[nodiscard]] TypeName ExpandAlias(TypeName type, std::span<const std::string> parameters = {});
  [[nodiscard]] TypeName DescribeType(TypeName type) const;
  std::string Specialize(const std::string& name,
                         const std::vector<TypeName>& arguments,
                         SourceLocation location);
  [[nodiscard]] Diagnostics TakeDiagnostics();
  [[nodiscard]] std::size_t function_count() const { return functions_.size(); }
  [[nodiscard]] std::size_t class_count() const { return classes_.size(); }
  [[nodiscard]] const Function& function(std::size_t index) const;
  [[nodiscard]] const Class& class_declaration(std::size_t index) const;
  void SetExpansionOwner(std::string_view name);
  void Publish();

 private:
  using Substitutions = absl::flat_hash_map<std::string, TypeName>;
  template <class T>
  struct Pending final {
    T declaration;
    Substitutions substitutions;
    const T* source = nullptr;
    std::size_t expansion_depth = 0;
  };
  struct Work final {
    enum class Kind : std::uint8_t { Function, Class };
    Kind kind;
    std::size_t index;
  };
  void Expand();
  void CollectDeclarations();
  void CollectAliases();
  void CollectFunctionTemplates();
  void CollectClassTemplates();
  void DescribeClassTemplate(const Class& owner);
  void NormalizeAliases();
  void Error(SourceLocation location, std::string message);
  Substitutions Bind(const std::vector<std::string>& parameters,
                     const std::vector<TypeName>& arguments,
                     SourceLocation location);
  void Normalize(TypeName& type, const Substitutions& substitutions);
  void NormalizeType(TypeName& type, const Substitutions& substitutions);
  void NormalizeTypeHead(TypeName& type);
  TypeName CloneNormalizedType(const TypeName& source, const Substitutions& substitutions);
  void NormalizeExpressionHead(Expression& expression,
                               const Substitutions& substitutions,
                               const Expression* object);
  TypeName TransformType(const TypeName& source) override;
  void TransformApplication(Expression& target, const Expression& source) override;
  void BeginScope() override;
  void EndScope() override;
  void Declare(const std::string& name) override;
  void Normalize(Expression* expression, const Substitutions& substitutions);
  void Normalize(Statement* statement, const Substitutions& substitutions);
  void Normalize(Function& function, const Substitutions& substitutions);
  void Normalize(Class& owner, const Substitutions& substitutions);
  void NormalizeScoped(Statement* statement, const Substitutions& substitutions);
  [[nodiscard]] bool IsShadowed(const std::string& name) const;
  std::string Instantiate(const std::string& name,
                          const std::vector<TypeName>& arguments,
                          SourceLocation location);

  Program& program_;
  Diagnostics diagnostics_;
  absl::flat_hash_map<std::string, Function> function_templates_;
  absl::flat_hash_map<std::string, Class> class_templates_;
  absl::flat_hash_map<std::string, Program::TypeAlias> aliases_;
  absl::flat_hash_set<std::string> instantiated_;
  absl::flat_hash_map<std::string, std::size_t> expansion_depths_;
  absl::flat_hash_map<std::string, TypeName> generic_types_;
  absl::flat_hash_set<std::string> active_aliases_;
  std::deque<Pending<Function>> functions_;
  std::deque<Pending<Class>> classes_;
  std::vector<Work> work_;
  std::vector<absl::flat_hash_set<std::string>> scopes_;
  std::size_t type_depth_ = 0;
  std::size_t expansion_depth_ = 0;
  const Substitutions* clone_substitutions_ = nullptr;
};

}  // namespace vela::frontend
