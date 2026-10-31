#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include "base/declaration.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/declaration_ids.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/syntax/ast_clone.h"

namespace vela::frontend {

struct GenericSpecialization final {
  std::string name;
  std::vector<TypeName> arguments;
  std::optional<Function::Application> method = std::nullopt;
};

// 用当前模块绑定重建存留特化，不能复用旧导入别名产生的扁平名字。
[[nodiscard]] std::expected<std::vector<GenericSpecialization>, Diagnostics>
RetainedSpecializations(const Program& program,
                        const DeclarationIds& identities,
                        std::span<const DeclarationId> declarations);

// 具体实参的名字与源码类型文本一致；含类型参数的抽象应用按参数身份区分同名参数。
[[nodiscard]] std::string SpecializationName(std::string_view base,
                                             std::span<const TypeName> arguments);

// 语义检查期间拥有特化工作表；模板、具体特化与抽象应用的声明地址在检查期间保持稳定。
// 类型实参由类型检查器求解并验证，本类只负责替换、规范化和唯一化声明。
// 实参含类型参数时产生抽象应用：它只为定义检查提供成员签名，不发布为可执行声明。
class Specializer final : public AstCloneTransform {
 public:
  // 特化与别名展开产生的类型应用；类型检查器在声明关系建立后验证其实参满足上界。
  struct Application final {
    std::string name;
    std::vector<TypeName> arguments;
    SourceLocation location;
  };
  struct MethodRequest final {
    std::string owner;
    DeclarationKind kind = DeclarationKind::Class;
    const Function* declaration = nullptr;
    std::vector<TypeName> arguments;
    SourceLocation location;
  };

  explicit Specializer(Program& program) : program_(program) {}
  ~Specializer() override = default;
  Specializer(const Specializer&) = delete;
  Specializer& operator=(const Specializer&) = delete;
  Specializer(Specializer&&) = delete;
  Specializer& operator=(Specializer&&) = delete;

  Diagnostics Initialize();
  Diagnostics Retain(std::span<const GenericSpecialization> retained);
  [[nodiscard]] const Function* FunctionTemplate(std::string_view name) const;
  [[nodiscard]] const Class* ClassTemplate(std::string_view name) const;
  [[nodiscard]] const Interface* InterfaceTemplate(std::string_view name) const;
  [[nodiscard]] const Program::TypeAlias* AliasTemplate(std::string_view name) const;
  [[nodiscard]] TypeName ExpandAlias(TypeName type);
  [[nodiscard]] TypeName DescribeType(TypeName type) const;
  std::string Specialize(const std::string& name,
                         const std::vector<TypeName>& arguments,
                         SourceLocation location);
  [[nodiscard]] std::expected<std::size_t, Diagnostic> SpecializeMethod(MethodRequest request);
  [[nodiscard]] Diagnostics TakeDiagnostics();
  [[nodiscard]] std::vector<Application> TakeApplications();
  [[nodiscard]] absl::flat_hash_map<std::string, TypeName> TakeTypeApplications();
  [[nodiscard]] std::size_t function_count() const { return functions_.size(); }
  [[nodiscard]] std::size_t class_count() const { return classes_.size(); }
  [[nodiscard]] std::size_t interface_count() const { return interfaces_.size(); }
  [[nodiscard]] std::size_t method_count() const { return methods_.size(); }
  [[nodiscard]] const Function& function(std::size_t index) const;
  [[nodiscard]] const Class& class_declaration(std::size_t index) const;
  [[nodiscard]] const Interface& interface_declaration(std::size_t index) const;
  [[nodiscard]] const Function& method(std::size_t index) const;
  [[nodiscard]] std::string_view method_owner(std::size_t index) const;
  [[nodiscard]] DeclarationKind method_owner_kind(std::size_t index) const;
  [[nodiscard]] const Function* method_source(std::size_t index) const;
  [[nodiscard]] bool IsAbstractClass(std::size_t index) const;
  [[nodiscard]] bool IsAbstractInterface(std::size_t index) const;
  // 具体特化来源的模板，用于关联定义诊断与具体执行声明。
  [[nodiscard]] const void* FunctionSource(std::size_t index) const;
  [[nodiscard]] const void* ClassSource(std::size_t index) const;
  void SetExpansionOwner(std::string_view name);
  void Publish();

 private:
  template <class T>
  struct Pending final {
    T declaration;
    TypeSubstitutions substitutions;
    const T* source = nullptr;
    std::size_t expansion_depth = 0;
    // 抽象应用由 Program::templates 拥有，不修改原模板的名称、签名或参数身份。
    T* abstract = nullptr;
  };
  struct Work final {
    enum class Kind : std::uint8_t { Function, Class, Interface, Method };
    Kind kind;
    std::size_t index;
  };
  struct PendingMethod final {
    std::string owner;
    DeclarationKind kind;
    Pending<Function> function;
  };
  void Expand();
  void ExpandFunction(Pending<Function>& pending);
  void ExpandClass(Pending<Class>& pending);
  void ExpandInterface(Pending<Interface>& pending);
  void CollectDeclarations();
  void CollectAliases();
  void CollectFunctionTemplates();
  void CollectClassTemplates();
  void CollectInterfaceTemplates();
  void DescribeClassTemplate(const Class& owner);
  void DescribeInterfaceTemplate(const Interface& owner);
  void DescribeParameters(DeclarationOrigin& origin, std::span<const TypeParameter> parameters);
  DeclarationOrigin::Member DescribeMethod(const Function& method, DeclarationKind kind);
  std::string DescribeFunctionContract(const Function& function);
  void NormalizeAliases();
  void Error(SourceLocation location, std::string message);
  TypeSubstitutions Bind(std::span<const TypeParameter> parameters,
                         std::span<const TypeName> arguments,
                         SourceLocation location);
  // 应用副本中的方法模板拥有新的参数身份，上界替换后的约束不会与模板或其它应用混淆。
  static void RenameMethodParameters(std::span<const Function> methods,
                                     const std::string& owner,
                                     TypeSubstitutions& substitutions);
  static void AssignMethodParameters(std::vector<Function>& methods, const std::string& owner);
  void RecordApplication(const std::string& name,
                         const std::vector<TypeName>& arguments,
                         SourceLocation location);
  void Normalize(TypeName& type, const TypeSubstitutions& substitutions);
  void NormalizeType(TypeName& type, const TypeSubstitutions& substitutions);
  void NormalizeTypeHead(TypeName& type);
  TypeName CloneNormalizedType(const TypeName& source, const TypeSubstitutions& substitutions);
  void NormalizeExpressionHead(Expression& expression,
                               const TypeSubstitutions& substitutions,
                               const Expression* object);
  TypeName TransformType(const TypeName& source) override;
  void TransformApplication(Expression& target, const Expression& source) override;
  void BeginScope() override;
  void EndScope() override;
  void Declare(const std::string& name) override;
  void Normalize(Expression* expression, const TypeSubstitutions& substitutions);
  void Normalize(Statement* statement, const TypeSubstitutions& substitutions);
  void Normalize(Function& function, const TypeSubstitutions& substitutions);
  void Normalize(Class& owner, const TypeSubstitutions& substitutions);
  void NormalizeScoped(Statement* statement, const TypeSubstitutions& substitutions);
  [[nodiscard]] bool IsShadowed(const std::string& name) const;
  [[nodiscard]] bool IsGenericDeclaration(std::string_view name) const;
  std::string Instantiate(const std::string& name,
                          const std::vector<TypeName>& arguments,
                          SourceLocation location);

  Program& program_;
  Diagnostics diagnostics_;
  std::vector<Application> applications_;
  // 模板节点由 Program::templates 拥有，应用只借用来源并创建独立声明。
  absl::flat_hash_map<std::string, Function*> function_templates_;
  absl::flat_hash_map<std::string, Class*> class_templates_;
  absl::flat_hash_map<std::string, Interface*> interface_templates_;
  absl::flat_hash_map<std::string, Program::TypeAlias> aliases_;
  absl::flat_hash_set<std::string> instantiated_;
  absl::flat_hash_map<std::string, std::size_t> expansion_depths_;
  absl::flat_hash_map<std::string, TypeName> generic_types_;
  absl::flat_hash_set<std::string> active_aliases_;
  std::deque<Pending<Function>> functions_;
  std::deque<Pending<Class>> classes_;
  std::deque<Pending<Interface>> interfaces_;
  std::deque<PendingMethod> methods_;
  absl::flat_hash_map<std::pair<std::string, std::string>, std::size_t> method_indices_;
  absl::flat_hash_map<std::string, Class*> class_declarations_;
  absl::flat_hash_map<std::string, Interface*> interface_declarations_;
  std::vector<Work> work_;
  std::vector<absl::flat_hash_set<std::string>> scopes_;
  std::size_t type_depth_ = 0;
  std::size_t expansion_depth_ = 0;
  const TypeSubstitutions* clone_substitutions_ = nullptr;
};

}  // namespace vela::frontend
