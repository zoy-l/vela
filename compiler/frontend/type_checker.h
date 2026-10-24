#pragma once

#include <cstdint>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include "base/builtin.h"
#include "compiler/frontend/ast.h"
#include "compiler/frontend/control_flow.h"
#include "compiler/frontend/diagnostic.h"

namespace vela::frontend {

using ConstantValue = std::variant<std::int64_t, bool, std::string>;

struct TypeDescriptor final {
  enum class Kind : std::uint8_t {
    Invalid,
    Null,
    Void,
    Bool,
    Int,
    Double,
    String,
    StringBuilder,
    Array,
    Enum,
    Class,
  };

  Kind kind = Kind::Invalid;
  std::string name;
  bool nullable = false;
};

struct FunctionDescriptor final {
  TypeDescriptor return_type;
  std::vector<std::string> parameter_order;
  absl::flat_hash_map<std::string, TypeDescriptor> parameters;
};

struct ClassDescriptor final {
  TypeDescriptor type;
  std::vector<std::string> field_order;
  absl::flat_hash_map<std::string, TypeDescriptor> fields;
  absl::flat_hash_map<std::string, FunctionDescriptor> methods;
};

struct SemanticModel final {
  absl::flat_hash_map<std::string, TypeDescriptor> types;
  // 借用受检 AST 的节点地址；模型只在该 AST 存活期间使用。
  absl::flat_hash_map<const Expression*, TypeDescriptor> expression_types;
  absl::flat_hash_map<const Expression*, ConstantValue> case_values;
  absl::flat_hash_map<std::string, std::int64_t> enum_values;
  absl::flat_hash_map<const Expression*, Builtin> builtin_calls;
  absl::flat_hash_map<const Statement*, TypeDescriptor> local_types;
  absl::flat_hash_map<const Statement*, StatementFlow> control_flow;
  absl::flat_hash_map<std::string, ClassDescriptor> classes;
  absl::flat_hash_map<std::string, FunctionDescriptor> functions;
};

struct TypeCheckResult final {
  Diagnostics diagnostics;
  std::size_t function_count = 0;
  SemanticModel model;
};

class TypeChecker final {
 public:
  [[nodiscard]] auto Check(const Program& program) -> TypeCheckResult;

 private:
  using Type = TypeDescriptor;
  using FunctionInfo = FunctionDescriptor;
  using ClassInfo = ClassDescriptor;
  struct LocalBinding final {
    Type type;
    BindingKind kind = BindingKind::Mutable;
    std::uint32_t id = 0;
  };
  using NullFacts = absl::flat_hash_set<std::uint32_t>;

  [[nodiscard]] static auto InvalidType() -> Type;
  void AddDiagnostic(SourceLocation location, std::string message);
  void CollectTypes(const Program& program);
  void ResolveDeclarations(const Program& program);
  void CheckFunction(const Function& function, const ClassInfo* owner);
  auto CheckStatement(const Statement& statement, const Type& return_type) -> StatementFlow;
  auto CheckBlock(const Statement& statement, const Type& return_type) -> StatementFlow;
  auto CheckSequence(const Statement& statement, const Type& return_type) -> StatementFlow;
  auto CheckScopedStatement(const Statement& statement, const Type& return_type) -> StatementFlow;
  auto CheckExpression(const Expression& expression, const Type* expected) -> Type;
  auto CheckExpressionType(const Expression& expression, const Type* expected) -> Type;
  auto CheckCall(const Expression& expression, const Type* expected) -> Type;
  auto CheckFunctionCall(const FunctionInfo& function,
                         const std::vector<CallArgument>& arguments,
                         SourceLocation location) -> Type;
  auto CheckClassConstruction(const ClassInfo& class_info,
                              const std::vector<CallArgument>& arguments,
                              SourceLocation location) -> Type;
  auto CheckMemberAccess(const Expression& expression) -> Type;
  auto ResolveType(const TypeName& type_name, bool allow_void) -> Type;
  auto LookupName(const Expression& expression) -> Type;
  [[nodiscard]] auto LookupLocal(std::string_view name) const -> const LocalBinding*;
  void BeginScope();
  void EndScope();
  auto DeclareLocal(std::string_view name,
                    const Type& type,
                    SourceLocation location,
                    BindingKind kind = BindingKind::Mutable) -> bool;
  [[nodiscard]] static auto IsAssignable(const Expression& expression) -> bool;
  [[nodiscard]] static auto IsSameType(const Type& left, const Type& right) -> bool;
  [[nodiscard]] static auto CanAssign(const Type& target, const Type& value) -> bool;
  [[nodiscard]] static auto NonNullable(Type type) -> Type;
  void RefineCondition(const Expression& condition, ConditionTruth outcome);
  void InvalidateAssignments(const Statement& statement);
  void InvalidateAssignments(const Expression& expression);
  [[nodiscard]] static auto IntersectFacts(const NullFacts& left, const NullFacts& right)
      -> NullFacts;
  [[nodiscard]] static auto IsNumeric(const Type& type) -> bool;
  [[nodiscard]] static auto IsValid(const Type& type) -> bool;
  [[nodiscard]] static auto TypeNameFor(const Type& type) -> std::string;

  void CheckVariableDeclaration(const Statement& statement);

  void CheckReturn(const Statement& statement, const Type& return_type);

  auto CheckIf(const Statement& statement, const Type& return_type) -> StatementFlow;

  auto CheckWhile(const Statement& statement, const Type& return_type) -> StatementFlow;

  auto CheckFor(const Statement& statement, const Type& return_type) -> StatementFlow;
  auto CheckSwitch(const Statement& statement, const Type& return_type) -> StatementFlow;
  [[nodiscard]] auto EvaluateConstant(const Expression& expression) const
      -> std::optional<ConstantValue>;
  [[nodiscard]] auto EvaluateUnaryConstant(const Expression& expression) const
      -> std::optional<ConstantValue>;
  [[nodiscard]] auto EvaluateBinaryConstant(const Expression& expression) const
      -> std::optional<ConstantValue>;
  void CheckCaseLabel(const Expression& label,
                      const Type& selector_type,
                      absl::flat_hash_set<std::string>& labels);

  auto CheckIntegerLiteral(const Expression& expression) -> Type;

  auto CheckIndex(const Expression& expression) -> Type;
  auto CheckArrayLiteral(const Expression& expression, const Type* expected) -> Type;

  auto CheckUnary(const Expression& expression) -> Type;

  auto CheckBinary(const Expression& expression) -> Type;
  auto CheckBinaryTypes(const Expression& expression, const Type& left, const Type& right) -> Type;
  auto CheckEquality(const Expression& expression, const Type& left, const Type& right) -> Type;

  auto ResolveSignature(const Function& function) -> FunctionInfo;
  auto CheckMethodCall(const Expression& expression, const Type* expected) -> Type;
  auto CheckBuiltinCall(const Expression& expression, Builtin builtin) -> Type;
  auto CheckAssignment(const Expression& expression) -> Type;

  Diagnostics diagnostics_;
  std::size_t function_count_ = 0;
  absl::flat_hash_map<std::string, Type> types_;
  absl::flat_hash_map<const Expression*, Type> expression_types_;
  absl::flat_hash_map<const Expression*, ConstantValue> case_values_;
  absl::flat_hash_map<std::uint32_t, ConstantValue> constant_bindings_;
  absl::flat_hash_map<std::string, std::int64_t> enum_values_;
  absl::flat_hash_map<const Expression*, Builtin> builtin_calls_;
  absl::flat_hash_map<const Statement*, Type> local_types_;
  absl::flat_hash_map<const Statement*, StatementFlow> control_flow_;
  absl::flat_hash_map<std::string, ClassInfo> classes_;
  absl::flat_hash_map<std::string, FunctionInfo> functions_;
  std::vector<absl::flat_hash_map<std::string, LocalBinding>> scopes_;
  std::size_t loop_depth_ = 0;
  std::size_t switch_depth_ = 0;
  std::uint32_t next_binding_id_ = 0;
  NullFacts non_null_;
  std::span<const std::string> source_paths_;
};

}  // namespace vela::frontend
